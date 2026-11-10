/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include "share/schema/ob_table_schema.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase { namespace share { namespace schema {
// Strings returned by this API borrow the schema's immutable comment storage.
struct AiColumnDefinition
{
  AiColumnDefinition() : column_id_(0), state_column_id_(0), request_column_id_(0),
    result_spec_column_id_(0), max_retries_(3), definition_version_(1), is_embedding_(false) {}
  uint64_t column_id_, state_column_id_, request_column_id_, result_spec_column_id_;
  common::ObString column_name_, model_name_, prompt_expr_, normalized_expr_;
  int64_t max_retries_, definition_version_;
  bool is_embedding_;
  common::ObSEArray<uint64_t, 4> dependency_column_ids_;
};

class AiColumnSchema
{
public:
  enum State { PENDING = 0, READY = 1, FAILED = 2 };
  static bool is_spec(const ObColumnSchemaV2 &column)
  {
    const common::ObString &s = column.get_comment_str();
    return column.is_hidden() && s.length() >= 6 && 0 == MEMCMP(s.ptr(), "AIC1|", 5);
  }
  static int encode(const AiColumnDefinition &d, common::ObSqlString &out)
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(out.append_fmt("AIC1|%lu|%lu|%lu|%lu|%ld|%ld|%d|%ld|",
        d.column_id_, d.state_column_id_, d.request_column_id_, d.result_spec_column_id_,
        d.max_retries_, d.definition_version_, d.is_embedding_ ? 1 : 0, d.dependency_column_ids_.count()))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < d.dependency_column_ids_.count(); ++i) {
      if (OB_FAIL(out.append_fmt("%lu|", d.dependency_column_ids_.at(i)))) {}
    }
    const common::ObString fields[] = {d.column_name_, d.model_name_, d.prompt_expr_, d.normalized_expr_};
    for (int i = 0; OB_SUCC(ret) && i < 4; ++i) {
      if (OB_FAIL(out.append_fmt("%d|", fields[i].length()))) {
      } else if (OB_FAIL(out.append(fields[i]))) {}
    }
    return ret;
  }
  static int decode(const common::ObString &text, AiColumnDefinition &d)
  {
    int ret = OB_SUCCESS;
    const char *p = text.ptr();
    const char *end = p + text.length();
    if (text.length() < 5 || 0 != MEMCMP(p, "AIC1|", 5)) {
      ret = OB_ENTRY_NOT_EXIST;
    } else {
      p += 5;
      uint64_t values[8] = {};
      for (int i = 0; OB_SUCC(ret) && i < 8; ++i) { ret = number(p, end, values[i]); }
      d.column_id_ = values[0]; d.state_column_id_ = values[1];
      d.request_column_id_ = values[2]; d.result_spec_column_id_ = values[3];
      d.max_retries_ = values[4]; d.definition_version_ = values[5]; d.is_embedding_ = values[6] != 0;
      d.dependency_column_ids_.reset();
      if (values[7] > OB_MAX_COLUMN_NUMBER) { ret = OB_INVALID_ARGUMENT; }
      for (uint64_t i = 0; OB_SUCC(ret) && i < values[7]; ++i) {
        uint64_t id = 0;
        if (OB_FAIL(number(p, end, id))) {} else if (OB_FAIL(d.dependency_column_ids_.push_back(id))) {}
      }
      common::ObString *fields[] = {&d.column_name_, &d.model_name_, &d.prompt_expr_, &d.normalized_expr_};
      for (int i = 0; OB_SUCC(ret) && i < 4; ++i) {
        uint64_t len = 0;
        if (OB_FAIL(number(p, end, len))) {
        } else if (len > static_cast<uint64_t>(end - p)) { ret = OB_INVALID_ARGUMENT;
        } else { fields[i]->assign_ptr(p, static_cast<int32_t>(len)); p += len; }
      }
      if (OB_SUCC(ret) && p != end) { ret = OB_INVALID_ARGUMENT; }
    }
    return ret;
  }
  static int get_definitions(const ObTableSchema &table, common::ObIArray<AiColumnDefinition> &out)
  {
    int ret = OB_SUCCESS;
    out.reset();
    for (ObTableSchema::const_column_iterator it = table.column_begin(); OB_SUCC(ret) && it != table.column_end(); ++it) {
      if (is_spec(**it)) {
        AiColumnDefinition d;
        if (OB_FAIL(decode((*it)->get_comment_str(), d))) {} else if (OB_FAIL(out.push_back(d))) {}
      }
    }
    return ret;
  }
  static int get_definition(const ObTableSchema &table, uint64_t id, AiColumnDefinition &out)
  {
    int ret = OB_ENTRY_NOT_EXIST;
    for (ObTableSchema::const_column_iterator it = table.column_begin(); it != table.column_end(); ++it) {
      if (is_spec(**it)) {
        AiColumnDefinition d;
        int tmp_ret = decode((*it)->get_comment_str(), d);
        if (OB_SUCCESS != tmp_ret) { ret = tmp_ret; break; }
        if (d.column_id_ == id) { out = d; ret = OB_SUCCESS; break; }
      }
    }
    return ret;
  }
  static bool is_ai_column(const ObTableSchema &table, uint64_t id)
  { AiColumnDefinition d; return OB_SUCCESS == get_definition(table, id, d); }
private:
  static int number(const char *&p, const char *end, uint64_t &n)
  {
    int ret = OB_SUCCESS;
    n = 0;
    if (p == end || *p < '0' || *p > '9') { ret = OB_INVALID_ARGUMENT; }
    while (OB_SUCC(ret) && p != end && *p >= '0' && *p <= '9') {
      const uint64_t digit = *p++ - '0';
      if (n > (UINT64_MAX - digit) / 10) { ret = OB_INVALID_ARGUMENT; } else { n = n * 10 + digit; }
    }
    if (OB_SUCC(ret)) {
      if (p == end || *p++ != '|') { ret = OB_INVALID_ARGUMENT; }
    }
    return ret;
  }
};
}}}
