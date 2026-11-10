// Standalone safety regression; compile with -DFILE_PARQUET_SAFETY_STANDALONE_TEST.
#ifdef FILE_PARQUET_SAFETY_STANDALONE_TEST
#include "sql/engine/file_source/file_parquet_reader.cpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace oceanbase::sql::file_source;
int main(int argc, char **argv)
{
  for (int width = 0; width <= 32; ++width) {
    std::vector<unsigned char> data(1 + 2 * width, 0);
    data[0] = 5; // two groups
    uint32_t mask = width == 32 ? UINT32_MAX : ((1ULL << width) - 1);
    for (int i = 0; i < 16; ++i) {
      const uint32_t value = i & mask;
      for (int b = 0; b < width; ++b) {
        if ((value >> b) & 1) { data[1 + (i * width + b) / 8] |= 1 << ((i * width + b) % 8); }
      }
    }
    std::vector<int32_t> values; size_t used = 0;
    assert(decode_rle_hybrid(data.data(), data.size(), width, 16, values, used));
    assert(used == data.size());
    for (int i = 0; i < 16; ++i) { assert(static_cast<uint32_t>(values[i]) == (i & mask)); }
    if (width > 0) { values.clear(); assert(!decode_rle_hybrid(data.data(), data.size()-1, width, 16, values, used)); }
  }
  std::vector<int32_t> values; size_t used;
  unsigned char bad[] = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x01};
  assert(!decode_rle_hybrid(bad, sizeof(bad), 1, 1, values, used));
  assert(!decode_rle_hybrid(bad, sizeof(bad), 33, 1, values, used));
  assert(!decode_rle_hybrid(bad, sizeof(bad), -1, 1, values, used));
  assert(!decode_rle_hybrid(bad, sizeof(bad), 1, -1, values, used));
  ParquetColumnDesc column; column.max_def_level=1; column.max_rep_level=0;
  PageHeaderInfo header; header.num_values=1;
  unsigned char truncated[] = {0xff,0xff,0xff,0x7f};
  std::vector<std::string> strings; std::vector<bool> nulls; std::string error;
  assert(!decode_data_page(truncated, sizeof(truncated), header, false, column, {}, strings, nulls, error));
  column.max_rep_level=1;
  assert(!decode_data_page(truncated, sizeof(truncated), header, false, column, {}, strings, nulls, error));
  std::vector<unsigned char> plain;
  assert(!snappy_uncompress(bad, sizeof(bad), plain));
  unsigned char oversized[] = {0x80,0x80,0x80,0x40};
  assert(!snappy_uncompress(oversized, sizeof(oversized), plain));
  char temp[]="/tmp/task2-safe-XXXXXX"; std::string dir=mkdtemp(temp);
  mkdir((dir+"/allowed").c_str(),0700); mkdir((dir+"/outside").c_str(),0700);
  std::ofstream(dir+"/outside/data.csv") << "secret";
  std::ofstream(dir+"/allowed/data.csv") << "safe";
  symlink((dir+"/outside").c_str(),(dir+"/allowed/escape").c_str());
  assert(!check_secure_path(dir+"/allowed",dir+"/allowed/escape/data.csv",error));
  assert(!check_secure_path(dir+"/allowed",dir+"/allowed/escape/../outside/data.csv",error));
  FILE *file=NULL; FileSignature sig;
  assert(!open_regular_file(dir+"/allowed/escape/data.csv",dir+"/allowed",file,sig).is_ok());
  assert(open_regular_file(dir+"/allowed/data.csv",dir+"/allowed",file,sig).is_ok());
  unlink((dir+"/allowed/data.csv").c_str());
  symlink((dir+"/outside/data.csv").c_str(),(dir+"/allowed/data.csv").c_str());
  char safe[5]={}; assert(fread(safe,1,4,file)==4); assert(std::string(safe)=="safe"); fclose(file);
  mkfifo((dir+"/allowed/fifo").c_str(),0600);
  assert(!open_regular_file(dir+"/allowed/fifo",dir+"/allowed",file,sig).is_ok());
  if (argc > 1) {
    FileParquetReader reader; FileReadOptions options;
    assert(reader.open(argv[1],options).is_ok());
    FileRow row; FileSourceError e; int count=0;
    while(reader.next_row(row,e)) { ++count; }
    assert(e.is_ok()); assert(count==7);
    options.projection_enabled=true;
    assert(reader.open(argv[1],options).is_ok()); count=0;
    while(reader.next_row(row,e)) { assert(row.nulls[0]); ++count; }
    assert(e.is_ok()); assert(count==7);
  }
  if (argc > 2) {
    FileParquetReader reader; FileReadOptions options;
    options.projection_enabled=true; options.projected_column_indices={0};
    assert(reader.open(argv[2],options).is_ok());
    FileRow row; FileSourceError e; int count=0;
    while(reader.next_row(row,e)) {
      assert(row.values.size()==2); assert(row.nulls[1]);
      assert(row.nulls[0] == (count % 2 == 0));
      if (!row.nulls[0]) { assert(row.values[0] == "g" + std::to_string(count/16) + ((count/2)%2 ? "b" : "a")); }
      ++count;
    }
    assert(e.is_ok()); assert(count==48);
    // The unprojected column has an intentionally corrupt dictionary header.
    assert(reader.open(std::string(argv[2])+".corrupt-unused",options).is_ok()); count=0;
    while(reader.next_row(row,e)) { ++count; }
    assert(e.is_ok()); assert(count==48);
    options.projection_enabled=false;
    assert(reader.open(argv[2],options).is_ok()); count=0;
    while(reader.next_row(row,e)) { assert(row.nulls[0]==row.nulls[1]); assert(row.values[0]==row.values[1]); ++count; }
    assert(e.is_ok()); assert(count==48);
    for (int unit=0; unit<4; ++unit) {
      e=reader.open(std::string(argv[2])+".logical"+std::to_string(unit),options);
      if (unit==3) { assert(e.code()==FileErrorCode::NOT_SUPPORTED); continue; }
      assert(e.is_ok());
      FileSchema schema; assert(reader.infer_schema(schema).is_ok());
      assert(schema.columns[0].type == (unit==0 ? ColumnType::DATE : ColumnType::DATETIME));
      assert(reader.next_row(row,e));
      assert(row.values[0] == (unit==0 ? "1970-01-02" : "1970-01-01 00:00:01"));
      assert(!reader.next_row(row,e)); assert(e.is_ok());
    }
  }
  std::cout << "Parquet/path safety tests passed\n";
}
#endif
