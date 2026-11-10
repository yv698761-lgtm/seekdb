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
#define USING_LOG_PREFIX SERVER
#include "observer/ai_service/ai_column_worker.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ai_column_schema.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "sql/engine/dml/ob_ai_column_dml.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/ob_tracepoint.h"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cmath>

namespace oceanbase
{
namespace observer
{
using namespace common;
using namespace share::schema;
namespace
{
std::string text(const ObString &s) { return s.empty() ? std::string() : std::string(s.ptr(), s.length()); }
std::string ident(const ObString &s)
{
  std::string out("`");
  for (int64_t i = 0; i < s.length(); ++i) {
    out += s.ptr()[i];
    if ('`' == s.ptr()[i]) { out += '`'; }
  }
  return out + '`';
}
// Hex literals do not depend on NO_BACKSLASH_ESCAPES or connection encoding.
std::string literal(const std::string &s)
{
  static const char digits[] = "0123456789abcdef";
  std::string out("CONVERT(X'");
  for (unsigned char ch : s) { out += digits[ch >> 4]; out += digits[ch & 15]; }
  return out + "' USING utf8mb4)";
}
std::string group_where(uint64_t table_id, uint64_t hash, const std::string &request)
{
  return "table_id=" + std::to_string(table_id) + " AND rowkey_hash=" + std::to_string(hash)
       + " AND request_id=" + literal(request);
}
std::string col_where(uint64_t table_id, uint64_t hash, const std::string &request, uint64_t col)
{
  return group_where(table_id, hash, request) + " AND column_id=" + std::to_string(col);
}
int get_string(sqlclient::ObMySQLResult &res, const char *name, std::string &out)
{
  ObString value;
  int ret = res.get_varchar(name, value);
  if (OB_SUCCESS == ret) { out = text(value); }
  return ret;
}
int table_name(const ObSchemaGetterGuard &guard, const ObTableSchema &table, std::string &name)
{
  const ObDatabaseSchema *db = nullptr;
  int ret = const_cast<ObSchemaGetterGuard &>(guard).get_database_schema(table.get_database_id(), db);
  if (OB_SUCCESS == ret && nullptr == db) { ret = OB_TABLE_NOT_EXIST; }
  if (OB_SUCCESS == ret) { name = ident(db->get_database_name_str()) + "." + ident(table.get_table_name_str()); }
  return ret;
}
}

AiColumnWorker::AiColumnWorker() : inited_(false) {}
AiColumnWorker::~AiColumnWorker() { stop(); wait(); }
int AiColumnWorker::init()
{
  int ret = OB_SUCCESS;
  if (inited_) { ret = OB_INIT_TWICE; }
  else if (OB_FAIL(set_thread_count(4))) { LOG_WARN("AI column thread configuration failed", K(ret)); }
  else { inited_ = true; }
  return ret;
}
void AiColumnWorker::run1()
{
  lib::set_thread_name("AiColumnWorker");
  while (!has_set_stop()) {
    if (nullptr != GCTX.sql_proxy_ && nullptr != GCTX.schema_service_
        && GCTX.schema_service_->is_runtime_schema_refreshed()) {
      int ret = poll();
      if (OB_SUCCESS != ret && OB_TABLE_NOT_EXIST != ret && OB_SCHEMA_EAGAIN != ret) {
        LOG_WARN("AI column worker poll failed", K(ret));
      }
    }
    // A short bounded sleep avoids spinning when no committed work exists.
    ob_usleep(100000);
  }
}
int AiColumnWorker::poll()
{
  int ret = OB_SUCCESS;
  Task task;
  bool found = false;
  {
    ObISQLClient::ReadResult result;
    const char *query = "SELECT table_id,column_id,rowkey_hash,request_id FROM oceanbase.__all_ai_column_task "
      "WHERE (state IN (0,3) AND eligible_ts<=NOW(6)) OR "
      "(state IN (1,2) AND (lease_expiry_ts IS NULL OR lease_expiry_ts<NOW(6))) "
      "ORDER BY eligible_ts LIMIT 1";
    if (OB_FAIL(GCTX.sql_proxy_->read(result, query))) { LOG_WARN("AI column read tasks failed", K(ret)); }
    else if (nullptr == result.get_result()) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(result.get_result()->next())) { if (OB_ITER_END == ret) { ret = OB_SUCCESS; } }
    else {
      sqlclient::ObMySQLResult &r = *result.get_result();
      if (OB_FAIL(r.get_uint("table_id", task.table_id))) {}
      else if (OB_FAIL(r.get_uint("column_id", task.column_id))) {}
      else if (OB_FAIL(r.get_uint("rowkey_hash", task.rowkey_hash))) {}
      else if (OB_FAIL(get_string(r, "request_id", task.request_id))) {}
      else { found = true; }
    }
  }
  if (OB_SUCC(ret) && found) {
    static std::atomic<uint64_t> sequence{0};
    std::string token = std::to_string(ObTimeUtility::current_time()) + "-" + std::to_string(++sequence);
    bool claimed = false;
    std::vector<Task> tasks;
    if (OB_FAIL(claim_group(task, token, claimed))) { LOG_WARN("AI column claim failed", K(ret)); }
    else if (!claimed) {}
    else if (OB_FAIL(read_group(task, token, tasks))) { LOG_WARN("AI column group read failed", K(ret)); }
    else {
      for (Task &item : tasks) {
        if (has_set_stop()) { break; }
        if (1 != item.state) {
          int tmp_ret = execute(item, token);
          if (OB_SUCCESS != tmp_ret) { LOG_WARN("AI column task execution failed", K(tmp_ret), K(item.table_id), K(item.column_id)); }
        }
      }
      if (!has_set_stop() && OB_FAIL(publish(task, token))) { LOG_WARN("AI column group publication failed", K(ret)); }
    }
  }
  return ret;
}
int AiColumnWorker::claim_group(const Task &task, const std::string &token, bool &claimed)
{
  int ret = OB_SUCCESS;
  int64_t affected = 0;
  // One conditional UPDATE owns the entire row-change group, never individual
  // columns. The NOT EXISTS predicate fences an already active owner.
  ObMySQLTransaction tx;
  std::string where = group_where(task.table_id, task.rowkey_hash, task.request_id);
  if (OB_FAIL(tx.start(GCTX.sql_proxy_))) {}
  else {
    ObISQLClient::ReadResult result;
    std::string lock = "SELECT state,IFNULL(lease_expiry_ts>NOW(6),0) AS active,"
        "IFNULL(eligible_ts>NOW(6),0) AS delayed FROM oceanbase.__all_ai_column_task WHERE "
        + where + " ORDER BY column_id FOR UPDATE";
    bool busy = false;
    if (OB_FAIL(tx.read(result, lock.c_str()))) {}
    else {
      sqlclient::ObMySQLResult *r = result.get_result();
      if (nullptr == r) { ret = OB_ERR_UNEXPECTED; }
      while (OB_SUCC(ret) && OB_SUCC(ret = r->next())) {
        int64_t state = 0, active = 0, delayed = 0;
        if (OB_FAIL(r->get_int("state", state))) {}
        else if (OB_FAIL(r->get_int("active", active))) {}
        else if (OB_FAIL(r->get_int("delayed", delayed))) {}
        else if ((2 == state && active) || ((0 == state || 3 == state) && delayed)) { busy = true; }
      }
      if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
    }
    result.close();
    if (OB_SUCC(ret) && !busy) {
      std::string query = "UPDATE oceanbase.__all_ai_column_task SET claim_token=" + literal(token)
        + ",lease_expiry_ts=NOW(6)+INTERVAL 10 MINUTE,state=IF(state=1,1,2) WHERE " + where;
      if (OB_FAIL(tx.write(query.c_str(), affected))) {}
      else { claimed = affected > 0; }
    }
  }
  if (tx.is_started()) { int e = tx.end(OB_SUCC(ret)); if (OB_SUCC(ret)) { ret = e; } }
  return ret;
}
int AiColumnWorker::read_group(const Task &base, const std::string &token, std::vector<Task> &tasks)
{
  int ret = OB_SUCCESS;
  ObISQLClient::ReadResult result;
  std::string query = "SELECT table_id,column_id,rowkey_hash,rowkey_predicate,request_id,definition_version,"
    "retry_count,state,prompt,IFNULL(result,'') AS result,(result IS NULL) AS result_null,error_msg FROM oceanbase.__all_ai_column_task WHERE "
    + group_where(base.table_id, base.rowkey_hash, base.request_id) + " AND claim_token=" + literal(token) + " ORDER BY column_id";
  if (OB_FAIL(GCTX.sql_proxy_->read(result, query.c_str()))) {}
  else {
    sqlclient::ObMySQLResult *r = result.get_result();
    if (nullptr == r) { ret = OB_ERR_UNEXPECTED; }
    while (OB_SUCC(ret) && OB_SUCC(ret = r->next())) {
      Task t;
      if (OB_FAIL(r->get_uint("table_id", t.table_id))) {}
      else if (OB_FAIL(r->get_uint("column_id", t.column_id))) {}
      else if (OB_FAIL(r->get_uint("rowkey_hash", t.rowkey_hash))) {}
      else if (OB_FAIL(r->get_int("definition_version", t.definition_version))) {}
      else if (OB_FAIL(r->get_int("retry_count", t.retry_count))) {}
      else if (OB_FAIL(r->get_int("state", t.state))) {}
      else if (OB_FAIL(get_string(*r, "rowkey_predicate", t.rowkey))) {}
      else if (OB_FAIL(get_string(*r, "request_id", t.request_id))) {}
      else if (OB_FAIL(get_string(*r, "prompt", t.prompt))) {}
      else if (OB_FAIL(get_string(*r, "result", t.result))) {}
      else if (OB_FAIL(get_string(*r, "error_msg", t.error))) {}
      else {
        int64_t null_result = 0;
        if (OB_FAIL(r->get_int("result_null", null_result))) {}
        else { t.result_null = (0 != null_result); tasks.push_back(t); }
      }
    }
    if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
  }
  return ret;
}
int AiColumnWorker::execute(Task &task, const std::string &token)
{
  int ret = OB_SUCCESS;
  // A prior retry checkpoint contains its old error and nullable result.
  // Neither is evidence about this inference attempt.
  task.error.clear();
  task.result.clear();
  task.result_null = true;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  AiColumnDefinition def;
  std::string qualified;
  sql::AiColumnDml::InternalGuard internal;
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {}
  else if (OB_FAIL(guard.get_table_schema(task.table_id, table))) {}
  else if (nullptr == table) { ret = OB_TABLE_NOT_EXIST; }
  else if (OB_FAIL(AiColumnSchema::get_definition(*table, task.column_id, def))) {}
  else if (def.definition_version_ != task.definition_version) { ret = OB_SCHEMA_EAGAIN; }
  else if (OB_FAIL(table_name(guard, *table, qualified))) {}
  else {
    const ObColumnSchemaV2 *req = table->get_column_schema(def.request_column_id_);
    const ObColumnSchemaV2 *col = table->get_column_schema(def.column_id_);
    if (nullptr == req || nullptr == col) { ret = OB_ERR_UNEXPECTED; }
    else {
      // Validate ownership before inference; publication checks it again under
      // the row lock, so even an ABA input update cannot accept a late response.
      std::string predicate = "(" + task.rowkey + ") AND " + ident(req->get_column_name_str()) + "=" + literal(task.request_id);
      bool current = false;
      {
        ObISQLClient::ReadResult result;
        std::string q = "SELECT 1 FROM " + qualified + " WHERE " + predicate;
        if (OB_FAIL(GCTX.sql_proxy_->read(result, q.c_str()))) {}
        else if (nullptr == result.get_result()) { ret = OB_ERR_UNEXPECTED; }
        else { int next = result.get_result()->next(); current = OB_SUCCESS == next; if (OB_ITER_END != next) { ret = next; } }
      }
      if (OB_SUCC(ret) && current) {
        ObISQLClient::ReadResult result;
        std::string expression = std::string(def.is_embedding_ ? "AI_EMBED(" : "AI_COMPLETE(")
          + literal(text(def.model_name_)) + "," + literal(task.prompt) + ")";
        std::string q = "SELECT CAST(" + expression + " AS CHAR) AS ai_result";
        if (OB_FAIL(GCTX.sql_proxy_->read(result, q.c_str()))) {}
        else if (nullptr == result.get_result()) { ret = OB_ERR_UNEXPECTED; }
        else if (OB_FAIL(result.get_result()->next())) {}
        else {
          ObString value;
          ret = result.get_result()->get_varchar("ai_result", value);
          if (OB_ERR_NULL_VALUE == ret) { ret = OB_SUCCESS; task.result_null = true; }
          else if (OB_SUCCESS == ret) { task.result = text(value); task.result_null = false; }
        }
        result.close();
        // A normal DML conversion is the authoritative type/dimension check.
        // Roll it back: no result/index entry is exposed before group publish.
        if (OB_SUCC(ret)) {
          if (!task.result_null && ob_is_numeric_type(col->get_data_type())) {
            char *end = nullptr; errno = 0;
            const char *start = task.result.c_str();
            long double n = std::strtold(start, &end);
            while (nullptr != end && (' ' == *end || '\t' == *end || '\n' == *end || '\r' == *end)) { ++end; }
            if (end == start || nullptr == end || '\0' != *end || ERANGE == errno || !std::isfinite(n)) { ret = OB_INVALID_ARGUMENT; }
          }
          if (OB_SUCC(ret)) {
            ObMySQLTransaction validation;
            int64_t affected = 0;
            std::string update = "UPDATE " + qualified + " SET " + ident(col->get_column_name_str()) + "="
              + (task.result_null ? "NULL" : literal(task.result)) + " WHERE " + predicate;
            if (OB_FAIL(validation.start(GCTX.sql_proxy_))) {}
            else if (OB_FAIL(validation.write(update.c_str(), affected))) {}
            if (validation.is_started()) { int e = validation.end(false); if (OB_SUCC(ret)) { ret = e; } }
          }
        }
      } else if (OB_SUCC(ret)) { task.error = "superseded"; }
    }
  }
  int execution_ret = ret;
  bool retry = OB_SUCCESS != execution_ret && nullptr != table
      && def.definition_version_ == task.definition_version
      && OB_SCHEMA_EAGAIN != execution_ret && OB_ENTRY_NOT_EXIST != execution_ret
      && task.retry_count < def.max_retries_;
  task.error = OB_SUCCESS == execution_ret ? task.error : std::to_string(execution_ret);
  std::string update = "UPDATE oceanbase.__all_ai_column_task SET state=" + std::string(retry ? "3" : "1")
    + ",retry_count=" + std::to_string(task.retry_count + (retry ? 1 : 0))
    + ",result=" + (OB_SUCCESS == execution_ret && !task.result_null ? literal(task.result) : "NULL")
    + ",error_msg=" + literal(task.error)
    // Keep the ownership token on every group member, even a retry. Publication
    // must observe the complete group, never only its already-terminal subset.
    + (retry ? ",eligible_ts=NOW(6)+INTERVAL 1 SECOND,lease_expiry_ts=NULL" : "")
    + " WHERE " + col_where(task.table_id, task.rowkey_hash, task.request_id, task.column_id)
    + " AND claim_token=" + literal(token) + " AND state=2";
  int64_t affected = 0;
  ret = GCTX.sql_proxy_->write(update.c_str(), affected);
  if (OB_FAIL(ret)) { LOG_WARN("AI column execution checkpoint failed", K(ret)); }
  return ret;
}
int AiColumnWorker::publish(const Task &base, const std::string &token)
{
  int ret = OB_SUCCESS;
  std::vector<Task> tasks;
  if (OB_FAIL(read_group(base, token, tasks))) { return ret; }
  if (tasks.empty()) { return ret; }
  for (const Task &t : tasks) { if (1 != t.state) { return OB_SUCCESS; } }
  sql::AiColumnDml::InternalGuard internal;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  std::string qualified;
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {}
  else if (OB_FAIL(guard.get_table_schema(base.table_id, table))) {}
  else if (nullptr != table && OB_FAIL(table_name(guard, *table, qualified))) {}
  ObMySQLTransaction tx;
  int64_t affected = 0;
  if (OB_SUCC(ret) && OB_FAIL(tx.start(GCTX.sql_proxy_))) {}
  if (OB_SUCC(ret)) {
    // Lock the tasks again and require every member to belong to this lease.
    // Lost owners may neither publish data nor remove a successor's tasks.
    ObISQLClient::ReadResult locked;
    std::string where = group_where(base.table_id, base.rowkey_hash, base.request_id);
    std::string lock = "SELECT state,claim_token FROM oceanbase.__all_ai_column_task WHERE " + where + " FOR UPDATE";
    int64_t count = 0;
    if (OB_FAIL(tx.read(locked, lock.c_str()))) {}
    else {
      sqlclient::ObMySQLResult *r = locked.get_result();
      if (nullptr == r) { ret = OB_ERR_UNEXPECTED; }
      while (OB_SUCC(ret) && OB_SUCC(ret = r->next())) {
        int64_t state = 0; std::string owner;
        if (OB_FAIL(r->get_int("state", state))) {}
        else if (OB_FAIL(get_string(*r, "claim_token", owner))) {}
        else if (1 != state || owner != token) { ret = OB_EAGAIN; }
        else { ++count; }
      }
      if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
      if (OB_SUCC(ret) && count != static_cast<int64_t>(tasks.size())) { ret = OB_EAGAIN; }
    }
    locked.close();
    if (OB_SUCC(ret) && nullptr != table) {
      std::string assignment, predicate = "(" + tasks.front().rowkey + ")";
      bool valid = true;
      for (const Task &t : tasks) {
        AiColumnDefinition def;
        if (OB_SUCCESS != AiColumnSchema::get_definition(*table, t.column_id, def)
            || def.definition_version_ != t.definition_version) { valid = false; break; }
        const ObColumnSchemaV2 *col = table->get_column_schema(def.column_id_);
        const ObColumnSchemaV2 *state = table->get_column_schema(def.state_column_id_);
        const ObColumnSchemaV2 *req = table->get_column_schema(def.request_column_id_);
        if (nullptr == col || nullptr == state || nullptr == req) { valid = false; break; }
        if (!assignment.empty()) { assignment += ','; }
        assignment += ident(col->get_column_name_str()) + "=" + (t.error.empty() && !t.result_null ? literal(t.result) : "NULL")
          + "," + ident(state->get_column_name_str()) + "=" + (t.error.empty() ? "1" : "2");
        predicate += " AND " + ident(req->get_column_name_str()) + "=" + literal(t.request_id);
      }
      if (valid && !assignment.empty()) {
        std::string q = "UPDATE " + qualified + " SET " + assignment + " WHERE " + predicate;
        if (OB_FAIL(tx.write(q.c_str(), affected))) { LOG_WARN("AI column grouped result write failed", K(ret)); }
      }
    }
    if (OB_SUCC(ret)) {
      std::string q = "DELETE FROM oceanbase.__all_ai_column_task WHERE " + where + " AND claim_token=" + literal(token);
      if (OB_FAIL(tx.write(q.c_str(), affected))) { LOG_WARN("AI column grouped task cleanup failed", K(ret)); }
    }
  }
  if (tx.is_started()) { int e = tx.end(OB_SUCC(ret)); if (OB_SUCC(ret)) { ret = e; } }
  return ret;
}
}
}
