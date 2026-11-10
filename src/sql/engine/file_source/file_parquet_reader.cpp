/**
 * Copyright (c) 2026 OceanBase
 *
 * OceanBase is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * OceanBase is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with OceanBase. If not, see <http://www.gnu.org/licenses/>.
 */

#include "sql/engine/file_source/file_parquet_reader.h"
#include "sql/engine/file_source/file_parquet_format.h"
#include "sql/engine/file_source/file_source_util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

namespace oceanbase {
namespace sql {
namespace file_source {

namespace {

const char *const kReaderParquetMagic = "PAR1";

// thrift compact protocol field types
const int8_t kReaderTypeBooleanTrue = 1;

// parquet physical types
const int32_t kReaderTypeBoolean = 0;
const int32_t kReaderTypeInt32 = 1;
const int32_t kReaderTypeInt64 = 2;
const int32_t kReaderTypeInt96 = 3;
const int32_t kReaderTypeFloat = 4;
const int32_t kReaderTypeDouble = 5;
const int32_t kReaderTypeByteArray = 6;
const int32_t kReaderTypeFixedLenByteArray = 7;

// parquet page types
const int32_t kReaderPageTypeData = 0;
const int32_t kReaderPageTypeIndex = 1;
const int32_t kReaderPageTypeDictionary = 2;
const int32_t kReaderPageTypeDataV2 = 3;

// parquet encodings
const int32_t kReaderEncodingPlain = 0;
const int32_t kReaderEncodingPlainDictionary = 2;
const int32_t kReaderEncodingRle = 3;
const int32_t kReaderEncodingRleDictionary = 8;

// converted types
const int32_t kReaderConvertedDate = 6;
const int32_t kReaderConvertedTimestampMillis = 9;
const int32_t kReaderConvertedTimestampMicros = 10;

// logical type kinds
const int32_t kReaderLogicalDate = 6;
const int32_t kReaderLogicalTimestamp = 8;

int32_t bit_width(int64_t max_value)
{
  int32_t width = 0;
  while ((1LL << width) <= max_value) {
    ++width;
  }
  return width;
}

/// Read a byte range of a file.
bool read_file_range(FILE *file, int64_t offset, int64_t length, std::vector<unsigned char> &buffer)
{
  if (offset < 0 || length < 0) {
    return false;
  }
  buffer.assign(static_cast<size_t>(length), 0);
  if (0 == length) {
    return true;
  }
  #if defined(_WIN32)
  if (0 != ::_fseeki64(file, offset, SEEK_SET)) {
#else
  if (0 != ::fseeko(file, static_cast<off_t>(offset), SEEK_SET)) {
#endif
    return false;
  }
  const size_t read = std::fread(&buffer[0], 1, static_cast<size_t>(length), file);
  return read == static_cast<size_t>(length);
}

/// Parsed page header.
struct PageHeaderInfo
{
  int32_t page_type = 0;
  int32_t uncompressed_page_size = 0;
  int32_t compressed_page_size = 0;
  int32_t num_values = 0;
  int32_t num_nulls = 0;
  int32_t encoding = 0;
  int32_t definition_levels_byte_length = 0;
  int32_t repetition_levels_byte_length = 0;
  bool is_compressed = true;
};

/// Read the schema element of the parquet schema.
bool read_schema_element(ThriftCompactReader &reader, ParquetSchemaNode &node)
{
  reader.push_struct();
  while (true) {
    int16_t field_id = 0;
    int8_t type = 0;
    bool is_stop = false;
    if (!reader.read_field_begin(field_id, type, is_stop)) {
      reader.pop_struct();
      return false;
    }
    if (is_stop) {
      reader.pop_struct();
      return true;
    }
    switch (field_id) {
      case 1: {
        int32_t value = 0;
        if (!reader.read_int32(value)) {
          return false;
        }
        node.type = value;
        break;
      }
      case 2: {
        int32_t value = 0;
        if (!reader.read_int32(value)) {
          return false;
        }
        node.type_length = value;
        break;
      }
      case 3: {
        int32_t value = 0;
        if (!reader.read_int32(value)) {
          return false;
        }
        node.repetition_type = value;
        break;
      }
      case 4: {
        std::string value;
        if (!reader.read_binary(value)) {
          return false;
        }
        node.name = value;
        break;
      }
      case 5: {
        int32_t value = 0;
        if (!reader.read_int32(value)) {
          return false;
        }
        node.num_children = value;
        break;
      }
      case 6: {
        int32_t value = 0;
        if (!reader.read_int32(value)) {
          return false;
        }
        node.converted_type = value;
        break;
      }
      case 10: {
        // the logical type is a union, the field id selects the member
        int16_t member_id = 0;
        int8_t member_type = 0;
        bool member_stop = false;
        reader.push_struct();
        if (!reader.read_field_begin(member_id, member_type, member_stop)) {
          reader.pop_struct();
          return false;
        }
        if (!member_stop) {
          node.logical_type_kind = member_id;
          if (member_id == kReaderLogicalTimestamp) {
            if (member_type != 12) { return false; }
            reader.push_struct();
            bool found_unit = false;
            while (true) {
              int16_t id = 0; int8_t type = 0; bool stop = false;
              if (!reader.read_field_begin(id, type, stop)) { return false; }
              if (stop) { break; }
              if (id == 2) {
                if (type != 12 || found_unit) { return false; }
                found_unit = true;
                reader.push_struct();
                int16_t unit = 0; int8_t unit_type = 0; bool unit_stop = false;
                if (!reader.read_field_begin(unit, unit_type, unit_stop) || unit_stop
                    || unit_type != 12 || unit < 1 || unit > 3
                    || !reader.skip_field(unit_type)) { return false; }
                node.timestamp_unit = unit;
                if (!reader.read_field_begin(unit, unit_type, unit_stop) || !unit_stop) { return false; }
                reader.pop_struct();
              } else if (!reader.skip_field(type)) { return false; }
            }
            reader.pop_struct();
            if (!found_unit) { return false; }
          } else if (!reader.skip_field(member_type)) {
            reader.pop_struct();
            return false;
          }
          // The logical type is itself a thrift struct, its field value is
          // followed by the stop field of the union. Consume it here,
          // otherwise the next schema field is read from the union stop byte.
          int16_t union_stop_id = 0;
          int8_t union_stop_type = 0;
          bool union_stop = false;
          if (!reader.read_field_begin(union_stop_id, union_stop_type, union_stop)
              || !union_stop) {
            reader.pop_struct();
            return false;
          }
        }
        reader.pop_struct();
        break;
      }
      default: {
        if (!reader.skip_field(type)) {
          return false;
        }
        break;
      }
    }
  }
}

/// Read a column chunk of a row group.
bool read_column_chunk(ThriftCompactReader &reader, ParquetColumnChunk &chunk)
{
  reader.push_struct();
  while (true) {
    int16_t field_id = 0;
    int8_t type = 0;
    bool is_stop = false;
    if (!reader.read_field_begin(field_id, type, is_stop)) {
      reader.pop_struct();
      return false;
    }
    if (is_stop) {
      reader.pop_struct();
      return true;
    }
    if (2 == field_id) {
      int64_t value = 0;
      if (!reader.read_int64(value)) {
        return false;
      }
      chunk.file_offset = value;
      continue;
    }
    if (3 == field_id) {
      // the column meta data
      reader.push_struct();
      while (true) {
        int16_t meta_id = 0;
        int8_t meta_type = 0;
        bool meta_stop = false;
        if (!reader.read_field_begin(meta_id, meta_type, meta_stop)) {
          reader.pop_struct();
          return false;
        }
        if (meta_stop) {
          break;
        }
        switch (meta_id) {
          case 1: {
            int32_t value = 0;
            if (!reader.read_int32(value)) {
              return false;
            }
            break;
          }
          case 2: {
            int8_t elem_type = 0;
            uint32_t count = 0;
            if (!reader.read_list_begin(elem_type, count)) {
              return false;
            }
            for (uint32_t i = 0; i < count; ++i) {
              int32_t value = 0;
              if (!reader.read_int32(value)) {
                return false;
              }
              chunk.encodings.push_back(value);
            }
            break;
          }
          case 3: {
            int8_t elem_type = 0;
            uint32_t count = 0;
            if (!reader.read_list_begin(elem_type, count)) {
              return false;
            }
            for (uint32_t i = 0; i < count; ++i) {
              std::string value;
              if (!reader.read_binary(value)) {
                return false;
              }
              if (i + 1 == count) {
                chunk.path_in_schema = value;
              }
            }
            break;
          }
          case 4: {
            int32_t value = 0;
            if (!reader.read_int32(value)) {
              return false;
            }
            chunk.codec = value;
            break;
          }
          case 5: {
            int64_t value = 0;
            if (!reader.read_int64(value)) {
              return false;
            }
            chunk.num_values = value;
            break;
          }
          case 6: {
            int64_t value = 0;
            if (!reader.read_int64(value)) {
              return false;
            }
            chunk.total_uncompressed_size = value;
            break;
          }
          case 7: {
            int64_t value = 0;
            if (!reader.read_int64(value)) {
              return false;
            }
            chunk.total_compressed_size = value;
            break;
          }
          case 9: {
            int64_t value = 0;
            if (!reader.read_int64(value)) {
              return false;
            }
            chunk.data_page_offset = value;
            break;
          }
          case 10:
          case 12:
          case 13:
          case 14:
          case 15:
          case 16:
          case 17:
          case 18: {
            if (!reader.skip_field(meta_type)) {
              return false;
            }
            break;
          }
          case 11: {
            int64_t value = 0;
            if (!reader.read_int64(value)) {
              return false;
            }
            chunk.dictionary_page_offset = value;
            break;
          }
          default: {
            if (!reader.skip_field(meta_type)) {
              reader.pop_struct();
              return false;
            }
            break;
          }
        }
      }
      reader.pop_struct();
      continue;
    }
    if (!reader.skip_field(type)) {
      return false;
    }
  }
}

/// Read a row group of the file metadata.
bool read_row_group(ThriftCompactReader &reader, ParquetRowGroup &row_group)
{
  reader.push_struct();
  while (true) {
    int16_t field_id = 0;
    int8_t type = 0;
    bool is_stop = false;
    if (!reader.read_field_begin(field_id, type, is_stop)) {
      reader.pop_struct();
      return false;
    }
    if (is_stop) {
      reader.pop_struct();
      return true;
    }
    if (1 == field_id) {
      int8_t elem_type = 0;
      uint32_t count = 0;
      if (!reader.read_list_begin(elem_type, count)) {
        return false;
      }
      for (uint32_t i = 0; i < count; ++i) {
        ParquetColumnChunk chunk;
        if (!read_column_chunk(reader, chunk)) {
          return false;
        }
        row_group.columns.push_back(chunk);
      }
      continue;
    }
    if (3 == field_id) {
      int64_t value = 0;
      if (!reader.read_int64(value)) {
        return false;
      }
      row_group.num_rows = value;
      continue;
    }
    if (2 == field_id) {
      int64_t value = 0;
      if (!reader.read_int64(value)) {
        return false;
      }
      row_group.total_byte_size = value;
      continue;
    }
    if (!reader.skip_field(type)) {
      return false;
    }
  }
}

/// Read the page header of a page.
bool read_page_header(ThriftCompactReader &reader, PageHeaderInfo &header)
{
  reader.push_struct();
  while (true) {
    int16_t field_id = 0;
    int8_t type = 0;
    bool is_stop = false;
    if (!reader.read_field_begin(field_id, type, is_stop)) {
      reader.pop_struct();
      return false;
    }
    if (is_stop) {
      reader.pop_struct();
      return true;
    }
    switch (field_id) {
      case 1: {
        int32_t value = 0;
        if (!reader.read_int32(value)) {
          return false;
        }
        header.page_type = value;
        break;
      }
      case 2: {
        int32_t value = 0;
        if (!reader.read_int32(value)) {
          return false;
        }
        header.uncompressed_page_size = value;
        break;
      }
      case 3: {
        int32_t value = 0;
        if (!reader.read_int32(value)) {
          return false;
        }
        header.compressed_page_size = value;
        break;
      }
      case 5: {
        // the data page header of a v1 page
        reader.push_struct();
        while (true) {
          int16_t page_id = 0;
          int8_t page_type = 0;
          bool page_stop = false;
          if (!reader.read_field_begin(page_id, page_type, page_stop)) {
            reader.pop_struct();
            return false;
          }
          if (page_stop) {
            break;
          }
          if (1 == page_id) {
            int32_t value = 0;
            if (!reader.read_int32(value)) {
              return false;
            }
            header.num_values = value;
            continue;
          }
          if (2 == page_id) {
            int32_t value = 0;
            if (!reader.read_int32(value)) {
              return false;
            }
            header.encoding = value;
            continue;
          }
          if (!reader.skip_field(page_type)) {
            reader.pop_struct();
            return false;
          }
        }
        reader.pop_struct();
        break;
      }
      case 7: {
        // the dictionary page header
        reader.push_struct();
        while (true) {
          int16_t page_id = 0;
          int8_t page_type = 0;
          bool page_stop = false;
          if (!reader.read_field_begin(page_id, page_type, page_stop)) {
            reader.pop_struct();
            return false;
          }
          if (page_stop) {
            break;
          }
          if (1 == page_id) {
            int32_t value = 0;
            if (!reader.read_int32(value)) {
              return false;
            }
            header.num_values = value;
            continue;
          }
          if (2 == page_id) {
            int32_t value = 0;
            if (!reader.read_int32(value)) {
              return false;
            }
            header.encoding = value;
            continue;
          }
          if (!reader.skip_field(page_type)) {
            reader.pop_struct();
            return false;
          }
        }
        reader.pop_struct();
        break;
      }
      case 8: {
        // the data page header of a v2 page
        reader.push_struct();
        while (true) {
          int16_t page_id = 0;
          int8_t page_type = 0;
          bool page_stop = false;
          if (!reader.read_field_begin(page_id, page_type, page_stop)) {
            reader.pop_struct();
            return false;
          }
          if (page_stop) {
            break;
          }
          switch (page_id) {
            case 1: {
              int32_t value = 0;
              if (!reader.read_int32(value)) {
                return false;
              }
              header.num_values = value;
              break;
            }
            case 2: {
              int32_t value = 0;
              if (!reader.read_int32(value)) {
                return false;
              }
              header.num_nulls = value;
              break;
            }
            case 4: {
              int32_t value = 0;
              if (!reader.read_int32(value)) {
                return false;
              }
              header.encoding = value;
              break;
            }
            case 5: {
              int32_t value = 0;
              if (!reader.read_int32(value)) {
                return false;
              }
              header.definition_levels_byte_length = value;
              break;
            }
            case 6: {
              int32_t value = 0;
              if (!reader.read_int32(value)) {
                return false;
              }
              header.repetition_levels_byte_length = value;
              break;
            }
            case 7: {
              // a boolean is encoded in the type of the field header
              header.is_compressed = (kReaderTypeBooleanTrue == page_type);
              break;
            }
            default: {
              if (!reader.skip_field(page_type)) {
                reader.pop_struct();
                return false;
              }
              break;
            }
          }
        }
        reader.pop_struct();
        break;
      }
      default: {
        if (!reader.skip_field(type)) {
          return false;
        }
        break;
      }
    }
  }
}

/// Decode the values of a data page.
///
/// `dictionary` holds the values of the dictionary page when the page uses a
/// dictionary encoding.
bool decode_data_page(const unsigned char *page,
                       size_t page_size,
                       const PageHeaderInfo &header,
                       bool is_v2,
                       const ParquetColumnDesc &column,
                       const std::vector<std::string> &dictionary,
                       std::vector<std::string> &values,
                       std::vector<bool> &nulls,
                       std::string &error)
{
  if (header.num_values < 0 || header.definition_levels_byte_length < 0
      || header.repetition_levels_byte_length < 0) {
    error = "invalid parquet page value count or level length";
    return false;
  }
  const int32_t max_def_level = column.max_def_level;
  const int32_t max_rep_level = column.max_rep_level;
  std::vector<int32_t> definition_levels;
  size_t payload_pos = 0;
  size_t payload_size = page_size;
  if (is_v2) {
    payload_pos = static_cast<size_t>(header.repetition_levels_byte_length)
                  + static_cast<size_t>(header.definition_levels_byte_length);
    if (payload_pos > page_size) {
      error = "the definition levels of the parquet page are truncated";
      return false;
    }
    payload_size = page_size - payload_pos;
    if (max_def_level > 0) {
      const unsigned char *def_data = page + header.repetition_levels_byte_length;
      size_t consumed = 0;
      if (!decode_rle_hybrid(def_data,
                             static_cast<size_t>(header.definition_levels_byte_length),
                             bit_width(max_def_level),
                             header.num_values,
                             definition_levels,
                             consumed)) {
        error = "failed to decode the definition levels of the parquet page";
        return false;
      }
    }
  } else {
    size_t pos = 0;
    if (max_rep_level > 0) {
      if (pos + 4 > page_size) {
        error = "the repetition levels of the parquet page are truncated";
        return false;
      }
      uint32_t length = 0;
      std::memcpy(&length, page + pos, 4);
      pos += 4;
      if (length > page_size - pos) {
        error = "the repetition levels of the parquet page are truncated";
        return false;
      }
      std::vector<int32_t> repetition_levels;
      size_t consumed = 0;
      if (!decode_rle_hybrid(page + pos, length, bit_width(max_rep_level), header.num_values, repetition_levels,
                             consumed)) {
        error = "failed to decode the repetition levels of the parquet page";
        return false;
      }
      pos += length;
    }
    if (max_def_level > 0) {
      if (pos + 4 > page_size) {
        error = "the definition levels of the parquet page are truncated";
        return false;
      }
      uint32_t length = 0;
      std::memcpy(&length, page + pos, 4);
      pos += 4;
      if (length > page_size - pos) {
        error = "the definition levels of the parquet page are truncated";
        return false;
      }
      size_t consumed = 0;
      if (!decode_rle_hybrid(page + pos, length, bit_width(max_def_level), header.num_values, definition_levels,
                             consumed)) {
        error = "failed to decode the definition levels of the parquet page";
        return false;
      }
      pos += length;
    }
    if (pos > page_size) {
      error = "the definition levels of the parquet page are truncated";
      return false;
    }
    payload_pos = pos;
    payload_size = page_size - pos;
  }
  for (int32_t level : definition_levels) {
    if (level < 0 || level > max_def_level) {
      error = "invalid parquet definition level";
      return false;
    }
  }
  // the number of values that are not null
  int64_t present = header.num_values;
  if (max_def_level > 0 && !definition_levels.empty()) {
    present = 0;
    for (size_t i = 0; i < definition_levels.size() && i < static_cast<size_t>(header.num_values); ++i) {
      if (definition_levels[i] >= max_def_level) {
        ++present;
      }
    }
  }
  const bool dictionary_encoded =
      (kReaderEncodingPlainDictionary == header.encoding || kReaderEncodingRleDictionary == header.encoding);
  std::vector<std::string> decoded;
  if (dictionary_encoded) {
    if (dictionary.empty() && present > 0) {
      error = "the parquet page uses a dictionary encoding but the file has no dictionary page";
      return false;
    }
    const unsigned char *index_data = page + payload_pos;
    size_t index_size = payload_size;
    if (index_size < 1 && present > 0) {
      error = "the dictionary indices of the parquet page are truncated";
      return false;
    }
    const int32_t width = index_size == 0 ? 0 : index_data[0];
    std::vector<int32_t> indices;
    size_t consumed = 0;
    if (present > 0 && !decode_rle_hybrid(index_data + 1, index_size - 1, width, present, indices, consumed)) {
      error = "failed to decode the dictionary indices of the parquet page";
      return false;
    }
    size_t decoded_bytes = 0;
    for (int64_t i = 0; i < present; ++i) {
      const int32_t index = indices[static_cast<size_t>(i)];
      if (index < 0 || static_cast<size_t>(index) >= dictionary.size()) {
        error = "the dictionary index of the parquet page is out of range";
        return false;
      }
      const std::string &value = dictionary[static_cast<size_t>(index)];
      if (value.size() > 64 * 1024 * 1024 - decoded_bytes) {
        error = "dictionary expansion exceeds bounded parquet page budget";
        return false;
      }
      decoded_bytes += value.size();
      decoded.push_back(value);
    }
  } else if (kReaderEncodingPlain == header.encoding) {
    if (!decode_plain_values(page + payload_pos,
                              payload_size,
                              column.type,
                              column.type_length,
                              column.converted_type,
                              present,
                              decoded)) {
      error = "failed to decode the plain encoded values of the parquet page";
      return false;
    }
  } else {
    error = "the parquet page uses the unsupported encoding " + std::to_string(header.encoding);
    return false;
  }
  // interleave the decoded values with the null flags of the page
  size_t value_index = 0;
  for (int64_t i = 0; i < header.num_values; ++i) {
    const bool is_null = (!definition_levels.empty()
                           && static_cast<size_t>(i) < definition_levels.size()
                           && definition_levels[static_cast<size_t>(i)] < max_def_level);
    if (is_null) {
      values.push_back(std::string());
      nulls.push_back(true);
      continue;
    }
    if (value_index < decoded.size()) {
      values.push_back(std::move(decoded[value_index++]));
    } else {
      values.push_back(std::string());
    }
    nulls.push_back(false);
  }
  return true;
}

}  // namespace

FileParquetReader::FileParquetReader() {}

FileParquetReader::~FileParquetReader() { if (file_ != NULL) { std::fclose(file_); } }

FileSourceError FileParquetReader::open(const std::string &path, const FileReadOptions &options)
{
  if (file_ != NULL) { std::fclose(file_); file_ = NULL; }
  opened_ = false;
  path_ = path;
  options_ = options;
  footer_.clear(); schema_nodes_.clear(); columns_.clear(); row_groups_.clear();
  cursors_.clear(); selected_columns_.clear();
  current_group_ = current_row_ = current_row_count_ = emitted_ = 0;
  FileSourceError error = open_regular_file(path, options.secure_file_priv, file_, signature_);
  if (!error.is_ok()) { return error; }
  error = read_footer();
  if (!error.is_ok()) { return error; }
  if (options.projection_enabled) {
    for (int64_t c : options.projected_column_indices) {
      if (c < 0 || static_cast<size_t>(c) >= columns_.size()) {
        return FileSourceError(FileErrorCode::SCHEMA_MISMATCH, "parquet projection index is out of range");
      }
      selected_columns_.push_back(static_cast<size_t>(c));
    }
    std::sort(selected_columns_.begin(), selected_columns_.end());
    selected_columns_.erase(std::unique(selected_columns_.begin(), selected_columns_.end()), selected_columns_.end());
  } else {
    for (size_t c = 0; c < columns_.size(); ++c) { selected_columns_.push_back(c); }
  }
  opened_ = true;
  return FileSourceError();
}

FileSourceError FileParquetReader::check_unchanged() const
{
  FileSignature current;
  if (!get_open_file_signature(file_, current)) {
    return FileSourceError(FileErrorCode::FILE_NOT_FOUND, "cannot stat file '" + path_ + "'");
  }
  if (!(current == signature_)) {
    return FileSourceError(FileErrorCode::FILE_CHANGED, "file '" + path_ + "' changed during the scan");
  }
  return FileSourceError();
}

FileSourceError FileParquetReader::read_footer()
{
  FILE *file = file_;
  FileSourceError result;
  std::vector<unsigned char> tail;
  if (!read_file_range(file, signature_.size - 8, 8, tail) || 0 != std::memcmp(&tail[4], kReaderParquetMagic, 4)) {
    return FileSourceError(FileErrorCode::PARSE_ERROR, "the file '" + path_ + "' is not a parquet file");
  }
  uint32_t footer_size = 0;
  std::memcpy(&footer_size, &tail[0], 4);
  if (footer_size > 64 * 1024 * 1024) {
    return FileSourceError(FileErrorCode::LIMIT_EXCEEDED, "parquet footer exceeds bounded reader budget");
  }
  if (static_cast<int64_t>(footer_size) + 12 > signature_.size) {
    return FileSourceError(FileErrorCode::PARSE_ERROR, "the footer of '" + path_ + "' is corrupted");
  }
  if (!read_file_range(file, signature_.size - 8 - footer_size, footer_size, footer_)) {
    return FileSourceError(FileErrorCode::IO_ERROR, "cannot read the footer of '" + path_ + "'");
  }

  ThriftCompactReader reader(footer_.empty() ? NULL : &footer_[0], footer_.size());
  reader.push_struct();
  while (true) {
    int16_t field_id = 0;
    int8_t type = 0;
    bool is_stop = false;
    if (!reader.read_field_begin(field_id, type, is_stop)) {
      reader.pop_struct();
      return FileSourceError(FileErrorCode::PARSE_ERROR, "the metadata of '" + path_ + "' is corrupted");
    }
    if (is_stop) {
      break;
    }
    if (2 == field_id) {
      // the schema
      int8_t elem_type = 0;
      uint32_t count = 0;
      if (!reader.read_list_begin(elem_type, count)) {
        return FileSourceError(FileErrorCode::PARSE_ERROR, "the schema of '" + path_ + "' is corrupted");
      }
      for (uint32_t i = 0; i < count; ++i) {
        ParquetSchemaNode node;
        if (!read_schema_element(reader, node)) {
          return FileSourceError(FileErrorCode::PARSE_ERROR, "the schema of '" + path_ + "' is corrupted");
        }
        schema_nodes_.push_back(node);
      }
      continue;
    }
    if (3 == field_id) {
      int64_t value = 0;
      if (!reader.read_int64(value)) {
        return FileSourceError(FileErrorCode::PARSE_ERROR, "the metadata of '" + path_ + "' is corrupted");
      }
      num_rows_ = value;
      continue;
    }
    if (4 == field_id) {
      int8_t elem_type = 0;
      uint32_t count = 0;
      if (!reader.read_list_begin(elem_type, count)) {
        return FileSourceError(FileErrorCode::PARSE_ERROR, "the row groups of '" + path_ + "' are corrupted");
      }
      for (uint32_t i = 0; i < count; ++i) {
        ParquetRowGroup row_group;
        if (!read_row_group(reader, row_group)) {
          return FileSourceError(FileErrorCode::PARSE_ERROR, "the row groups of '" + path_ + "' are corrupted");
        }
        row_groups_.push_back(row_group);
      }
      continue;
    }
    if (!reader.skip_field(type)) {
      reader.pop_struct();
      return FileSourceError(FileErrorCode::PARSE_ERROR, "the metadata of '" + path_ + "' is corrupted");
    }
  }
  reader.pop_struct();
  if (schema_nodes_.empty() || schema_nodes_[0].num_children < 0
      || static_cast<size_t>(schema_nodes_[0].num_children) != schema_nodes_.size() - 1) {
    return FileSourceError(FileErrorCode::NOT_SUPPORTED, "only flat parquet schemas are supported");
  }
  int64_t total_rows = 0;
  for (const ParquetRowGroup &group : row_groups_) {
    if (group.num_rows < 0 || group.num_rows > INT64_MAX - total_rows) {
      return FileSourceError(FileErrorCode::PARSE_ERROR, "invalid parquet row group row count");
    }
    total_rows += group.num_rows;
  }
  if (num_rows_ < 0 || total_rows != num_rows_) {
    return FileSourceError(FileErrorCode::PARSE_ERROR, "parquet footer row count does not match row groups");
  }
  // build the leaf columns of the file
  columns_.clear();
  for (size_t i = 0; i < schema_nodes_.size(); ++i) {
    const ParquetSchemaNode &node = schema_nodes_[i];
    if (i > 0 && (node.num_children != 0 || node.type < 0
                  || node.repetition_type == static_cast<int32_t>(ParquetRepetition::REPEATED))) {
      return FileSourceError(FileErrorCode::NOT_SUPPORTED, "nested/repeated parquet columns are not supported");
    }
    if (i == 0) {
      // the root element is never a column
      continue;
    }
    ParquetColumnDesc column;
    column.name = node.name;
    column.type = node.type;
    column.type_length = node.type_length;
    column.repetition_type = node.repetition_type;
    column.converted_type = node.converted_type;
    column.logical_type_kind = node.logical_type_kind;
    if (node.logical_type_kind == kReaderLogicalDate) {
      if (node.type != kReaderTypeInt32 || (node.converted_type != -1 && node.converted_type != kReaderConvertedDate)) {
        return FileSourceError(FileErrorCode::PARSE_ERROR, "inconsistent parquet DATE annotations");
      }
      column.converted_type = kReaderConvertedDate;
    } else if (node.logical_type_kind == kReaderLogicalTimestamp) {
      if (node.timestamp_unit == 3) {
        return FileSourceError(FileErrorCode::NOT_SUPPORTED, "nanosecond parquet timestamps are not supported");
      }
      const int32_t converted = node.timestamp_unit == 1 ? kReaderConvertedTimestampMillis : kReaderConvertedTimestampMicros;
      if (node.type != kReaderTypeInt64 || node.timestamp_unit < 1 || node.timestamp_unit > 2
          || (node.converted_type != -1 && node.converted_type != converted)) {
        return FileSourceError(FileErrorCode::PARSE_ERROR, "inconsistent parquet TIMESTAMP annotations");
      }
      column.converted_type = converted;
    }
    column.max_def_level = (static_cast<int32_t>(ParquetRepetition::OPTIONAL) == node.repetition_type) ? 1 : 0;
    column.max_rep_level = (static_cast<int32_t>(ParquetRepetition::REPEATED) == node.repetition_type) ? 1 : 0;
    columns_.push_back(column);
  }
  if (columns_.empty()) {
    return FileSourceError(FileErrorCode::PARSE_ERROR, "the file '" + path_ + "' does not contain any column");
  }
  return result;
}

FileSourceError FileParquetReader::infer_schema(FileSchema &schema)
{
  schema.columns.clear();
  for (size_t i = 0; i < columns_.size(); ++i) {
    const ParquetColumnDesc &column = columns_[i];
    FileColumn file_column;
    file_column.name = column.name;
    file_column.nullable = (static_cast<int32_t>(ParquetRepetition::OPTIONAL) == column.repetition_type);
    switch (column.type) {
      case kReaderTypeBoolean: file_column.type = ColumnType::BOOLEAN; break;
      case kReaderTypeInt32: {
        file_column.type = (kReaderConvertedDate == column.converted_type || kReaderLogicalDate == column.logical_type_kind)
                               ? ColumnType::DATE
                               : ColumnType::BIGINT;
        break;
      }
      case kReaderTypeInt64: {
        file_column.type = (kReaderConvertedTimestampMillis == column.converted_type
                            || kReaderConvertedTimestampMicros == column.converted_type
                            || kReaderLogicalTimestamp == column.logical_type_kind)
                               ? ColumnType::DATETIME
                               : ColumnType::BIGINT;
        break;
      }
      case kReaderTypeFloat:
      case kReaderTypeDouble: file_column.type = ColumnType::DOUBLE; break;
      case kReaderTypeByteArray:
      case kReaderTypeFixedLenByteArray: file_column.type = ColumnType::VARCHAR; break;
      default: file_column.type = ColumnType::VARCHAR; break;
    }
    schema.columns.push_back(file_column);
  }
  schema_ = schema;
  return FileSourceError();
}

FileSourceError FileParquetReader::start_row_group(int64_t index)
{
  const ParquetRowGroup &group = row_groups_[static_cast<size_t>(index)];
  if (group.num_rows < 0 || group.columns.size() != columns_.size()) {
    return FileSourceError(FileErrorCode::PARSE_ERROR, "invalid parquet row group shape");
  }
  cursors_.clear();
  cursors_.resize(columns_.size());
  for (size_t c : selected_columns_) {
    const ParquetColumnChunk &chunk = group.columns[c];
    if (columns_[c].max_rep_level != 0 || chunk.num_values != group.num_rows) {
      return FileSourceError(FileErrorCode::NOT_SUPPORTED, "nested/repeated parquet columns are not supported");
    }
    ColumnCursor &cursor = cursors_[c];
    cursor.offset = chunk.dictionary_page_offset >= 0 ? chunk.dictionary_page_offset : chunk.data_page_offset;
    if (cursor.offset < 4 || chunk.total_compressed_size <= 0
        || cursor.offset > signature_.size - chunk.total_compressed_size) {
      return FileSourceError(FileErrorCode::PARSE_ERROR, "invalid parquet column chunk bounds");
    }
    cursor.end = cursor.offset + chunk.total_compressed_size;
    if (chunk.codec != static_cast<int32_t>(ParquetCodec::SNAPPY)
        && chunk.codec != static_cast<int32_t>(ParquetCodec::UNCOMPRESSED)) {
      return FileSourceError(FileErrorCode::NOT_SUPPORTED, "unsupported parquet compression codec");
    }
  }
  current_row_ = 0;
  current_row_count_ = group.num_rows;
  return FileSourceError();
}

FileSourceError FileParquetReader::read_column_page(size_t c)
{
  ColumnCursor &cursor = cursors_[c];
  const ParquetColumnDesc &column = columns_[c];
  const ParquetColumnChunk &chunk = row_groups_[static_cast<size_t>(current_group_ - 1)].columns[c];
  cursor.values.clear(); cursor.nulls.clear(); cursor.row = 0;
  // Hard resource bounds are independent of row-group/chunk sizes. A malformed
  // page may not force unbounded header, decompression or string allocations.
  const int64_t max_page_bytes = 64LL * 1024 * 1024;
  const int64_t max_page_values = 1024 * 1024;
  while (cursor.offset < cursor.end && cursor.values_read < chunk.num_values) {
    PageHeaderInfo header;
    size_t header_size = 0;
    bool parsed = false;
    // Headers have no length prefix. Grow a bounded lookahead and never read
    // an entire chunk just to discover the next page header.
    for (int64_t lookahead = 256; ; lookahead *= 2) {
      const int64_t length = std::min<int64_t>(lookahead, cursor.end - cursor.offset);
      std::vector<unsigned char> header_bytes;
      if (!read_file_range(file_, cursor.offset, length, header_bytes)) {
        return FileSourceError(FileErrorCode::IO_ERROR, "cannot read parquet page header");
      }
      ThriftCompactReader reader(header_bytes.data(), header_bytes.size());
      if (read_page_header(reader, header)) { header_size = reader.position(); parsed = true; break; }
      if (lookahead >= 64 * 1024 || length < lookahead) { break; }
    }
    if (!parsed || header_size == 0 || header.compressed_page_size < 0
        || header.uncompressed_page_size < 0 || header.num_values < 0
        || header.definition_levels_byte_length < 0 || header.repetition_levels_byte_length < 0) {
      return FileSourceError(FileErrorCode::PARSE_ERROR, "invalid parquet page header");
    }
    cursor.offset += header_size;
    if (header.compressed_page_size > cursor.end - cursor.offset) {
      return FileSourceError(FileErrorCode::PARSE_ERROR, "truncated parquet page");
    }
    if (header.compressed_page_size > max_page_bytes || header.uncompressed_page_size > max_page_bytes
        || header.num_values > max_page_values) {
      return FileSourceError(FileErrorCode::LIMIT_EXCEEDED, "parquet page exceeds bounded reader budget");
    }
    std::vector<unsigned char> page;
    if (!read_file_range(file_, cursor.offset, header.compressed_page_size, page)) {
      return FileSourceError(FileErrorCode::IO_ERROR, "cannot read parquet page");
    }
    cursor.offset += header.compressed_page_size;
    if ((header.page_type == kReaderPageTypeData || header.page_type == kReaderPageTypeDataV2)
        && header.num_values > chunk.num_values - cursor.values_read) {
      return FileSourceError(FileErrorCode::PARSE_ERROR, "parquet page exceeds column value count");
    }
      if (kReaderPageTypeDictionary == header.page_type) {
        // the dictionary page holds the plain encoded dictionary values
        std::vector<unsigned char> plain;
        if (static_cast<int32_t>(ParquetCodec::SNAPPY) == chunk.codec) {
          if (!snappy_uncompress(page.empty() ? NULL : &page[0], page.size(), plain)) {
            return FileSourceError(FileErrorCode::PARSE_ERROR, "the dictionary page of '" + path_ + "' is corrupted");
          }
        } else {
          plain = page;
        }
        cursor.dictionary.clear();
        if (!decode_plain_values(plain.empty() ? NULL : &plain[0],
                                 plain.size(),
                                 column.type,
                                 column.type_length,
                                 column.converted_type,
                                 header.num_values,
                                 cursor.dictionary)) {
          return FileSourceError(FileErrorCode::PARSE_ERROR, "the dictionary page of '" + path_ + "' is corrupted");
        }
        continue;
      }
      if (kReaderPageTypeData != header.page_type && kReaderPageTypeDataV2 != header.page_type) {
        // index pages and unknown pages are ignored
        continue;
      }
      const bool is_v2 = (kReaderPageTypeDataV2 == header.page_type);
      std::vector<unsigned char> payload;
      if (is_v2) {
        const size_t levels_size = static_cast<size_t>(header.repetition_levels_byte_length)
                                   + static_cast<size_t>(header.definition_levels_byte_length);
        if (levels_size > page.size()) {
          return FileSourceError(FileErrorCode::PARSE_ERROR, "the page of '" + path_ + "' is truncated");
        }
        const unsigned char *data = page.data() + levels_size;
        const size_t data_size = page.size() - levels_size;
        if (static_cast<int32_t>(ParquetCodec::SNAPPY) == chunk.codec && header.is_compressed) {
          if (!snappy_uncompress(data, data_size, payload)) {
            return FileSourceError(FileErrorCode::PARSE_ERROR, "the page of '" + path_ + "' is corrupted");
          }
        } else {
          payload.assign(data, data + data_size);
        }
        // rebuild the page with the uncompressed values, the levels are kept
        std::vector<unsigned char> rebuilt;
        rebuilt.reserve(page.size());
        rebuilt.insert(rebuilt.end(), page.begin(), page.begin() + static_cast<long>(levels_size));
        rebuilt.insert(rebuilt.end(), payload.begin(), payload.end());
        std::string error;
        if (!decode_data_page(rebuilt.data(),
                              rebuilt.size(),
                              header,
                              true,
                              column,
                              cursor.dictionary,
                              cursor.values,
                              cursor.nulls,
                              error)) {
          return FileSourceError(FileErrorCode::PARSE_ERROR, error);
        }
      } else {
        std::vector<unsigned char> payload;
        if (static_cast<int32_t>(ParquetCodec::SNAPPY) == chunk.codec) {
          if (!snappy_uncompress(page.empty() ? NULL : &page[0], page.size(), payload)) {
            return FileSourceError(FileErrorCode::PARSE_ERROR, "the page of '" + path_ + "' is corrupted");
          }
        } else {
          payload = page;
        }
        std::string error;
        if (!decode_data_page(payload.empty() ? NULL : &payload[0],
                              payload.size(),
                              header,
                              false,
                              column,
                              cursor.dictionary,
                              cursor.values,
                              cursor.nulls,
                              error)) {
          return FileSourceError(FileErrorCode::PARSE_ERROR, error);
        }
      }
    cursor.values_read += header.num_values;
    for (const std::string &value : cursor.values) {
      if (options_.max_field_size > 0 && value.size() > static_cast<size_t>(options_.max_field_size)) {
        return FileSourceError(FileErrorCode::LIMIT_EXCEEDED, "parquet field exceeds max_field_size");
      }
    }
    if (!cursor.values.empty()) { return FileSourceError(); }
  }
  return FileSourceError(FileErrorCode::PARSE_ERROR, "parquet column ended before row group");
}

bool FileParquetReader::next_row(FileRow &row, FileSourceError &error)
{
  if (!opened_) {
    error = FileSourceError(FileErrorCode::FILE_NOT_FOUND, "the parquet file was not opened");
    return false;
  }
  while (current_row_ >= current_row_count_) {
    if (current_group_ >= static_cast<int64_t>(row_groups_.size())) { error = check_unchanged(); return false; }
    error = start_row_group(current_group_++);
    if (!error.is_ok()) { return false; }
  }
  if (options_.max_row_count > 0 && emitted_ >= options_.max_row_count) {
    error = FileSourceError(FileErrorCode::LIMIT_EXCEEDED, "the row limit of the file scan was exceeded");
    return false;
  }
  row.values.assign(columns_.size(), std::string());
  row.nulls.assign(columns_.size(), true);
  for (size_t c : selected_columns_) {
    ColumnCursor &cursor = cursors_[c];
    if (cursor.row >= cursor.values.size()) {
      error = read_column_page(c);
      if (!error.is_ok()) { return false; }
    }
    row.values[c] = std::move(cursor.values[cursor.row]);
    row.nulls[c] = cursor.nulls[cursor.row++];
  }
  ++current_row_; ++emitted_;
  error = FileSourceError();
  return true;
}

FileSourceError FileParquetReader::scan(const FileRowCallback &callback)
{
  current_group_ = current_row_ = current_row_count_ = emitted_ = 0;
  cursors_.clear();
  FileRow row;
  FileSourceError error;
  while (next_row(row, error)) { if (!callback(row)) { return FileSourceError(); } }
  return error;
}

}  // namespace file_source
}  // namespace sql
}  // namespace oceanbase
