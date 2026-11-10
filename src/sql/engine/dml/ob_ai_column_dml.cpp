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
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ob_ai_column_dml.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/resolver/ddl/ob_alter_table_stmt.h"
#include "share/schema/ai_column_schema.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
using namespace share::schema;
namespace sql
{
namespace
{
thread_local int64_t ai_column_internal_depth = 0;

int quote_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if ('`' == name.ptr()[i]) { OZ(sql.append("``")); }
    else { OZ(sql.append(name.ptr() + i, 1)); }
  }
  OZ(sql.append("`"));
  return ret;
}

int quote_string(ObSqlString &sql, const ObString &value)
{
  // Hex literals are independent of NO_BACKSLASH_ESCAPES and connection charset.
  int ret = sql.append("X'");
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    OZ(sql.append_fmt("%02X", static_cast<unsigned char>(value.ptr()[i])));
  }
  OZ(sql.append("'"));
  return ret;
}

int table_name(ObExecContext &ctx, const ObTableSchema &table, ObSqlString &name)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *db = nullptr;
  OZ(ctx.get_sql_ctx()->schema_guard_->get_database_schema(table.get_database_id(), db));
  CK(nullptr != db);
  if (OB_SUCC(ret)) {
    OZ(quote_identifier(name, db->get_database_name_str()));
    OZ(name.append("."));
    OZ(quote_identifier(name, table.get_table_name_str()));
  }
  return ret;
}

int write_sql(ObISQLConnection &conn, const ObSqlString &sql)
{
  int64_t affected = 0;
  return conn.execute_write(sql.ptr(), affected, true);
}

int row_predicate(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef,
                  const ObTableSchema &table, const ExprFixedArray &row,
                  ObSqlString &predicate)
{
  int ret = OB_SUCCESS;
  const ObRowkeyInfo &key = table.get_rowkey_info();
  for (int64_t i = 0; OB_SUCC(ret) && i < key.get_size(); ++i) {
    uint64_t cid = OB_INVALID_ID;
    int64_t idx = -1;
    OZ(key.get_column_id(i, cid));
    for (int64_t j = 0; j < ctdef.column_ids_.count(); ++j) {
      if (ctdef.column_ids_.at(j) == cid) { idx = j; break; }
    }
    const ObColumnSchemaV2 *column = table.get_column_schema(cid);
    CK(idx >= 0 && idx < row.count() && nullptr != column);
    if (OB_SUCC(ret)) {
      ObDatum *datum = nullptr;
      ObObj obj;
      char *buf = nullptr;
      int64_t len = 0;
      int64_t pos = 0;
      OZ(row.at(idx)->eval(op.get_eval_ctx(), datum));
      OZ(datum->to_obj(obj, row.at(idx)->obj_meta_));
      OZ(obj.print_sql_literal(buf, len, pos, op.get_exec_ctx().get_allocator(),
                              ObObjPrintParams(TZ_INFO(op.get_exec_ctx().get_my_session()))));
      if (i > 0) { OZ(predicate.append(" AND ")); }
      OZ(quote_identifier(predicate, column->get_column_name_str()));
      OZ(predicate.append(" = "));
      OZ(predicate.append(buf, pos));
    }
  }
  return ret;
}

int remove_tasks(ObISQLConnection &conn, uint64_t tid, const ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  OZ(sql.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND rowkey_predicate=", tid));
  OZ(quote_string(sql, predicate));
  OZ(write_sql(conn, sql));
  return ret;
}

int enqueue(ObISQLConnection &conn, const ObTableSchema &table,
            const AiColumnDefinition &def, const ObString &name,
            const ObString &predicate, const ObString &request)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  OZ(sql.append("INSERT INTO oceanbase.__all_ai_column_task "
                "(table_id,rowkey_hash,column_id,rowkey_predicate,request_id,definition_version,"
                "state,retry_count,eligible_ts,prompt) SELECT "));
  OZ(sql.append_fmt("%lu,MOD(CRC32(", table.get_table_id()));
  OZ(quote_string(sql, predicate));
  OZ(sql.append_fmt("),1000000007),%lu,", def.column_id_));
  OZ(quote_string(sql, predicate));
  OZ(sql.append(","));
  OZ(quote_string(sql, request));
  OZ(sql.append_fmt(",%ld,0,0,NOW(6)-INTERVAL 1 SECOND,(%.*s) FROM %.*s WHERE %.*s "
                    "ON DUPLICATE KEY UPDATE rowkey_predicate=VALUES(rowkey_predicate),"
                    "request_id=VALUES(request_id),definition_version=VALUES(definition_version),"
                    "state=0,retry_count=0,eligible_ts=VALUES(eligible_ts),lease_expiry_ts=NULL,"
                    "claim_token='',prompt=VALUES(prompt),result=NULL,error_msg=''",
                    def.definition_version_, def.prompt_expr_.length(), def.prompt_expr_.ptr(),
                    name.length(), name.ptr(), predicate.length(), predicate.ptr()));
  OZ(write_sql(conn, sql));
  return ret;
}
}

AiColumnDml::InternalGuard::InternalGuard() { ++ai_column_internal_depth; }
AiColumnDml::InternalGuard::~InternalGuard() { --ai_column_internal_depth; }
bool AiColumnDml::is_internal() { return ai_column_internal_depth > 0; }

bool AiColumnDml::needs_after_row(ObExecContext &ctx, const ObDMLBaseCtDef &ctdef)
{
  bool needed = false;
  if (!is_internal() && ctdef.is_primary_index_ && nullptr != ctx.get_sql_ctx()
      && nullptr != ctx.get_sql_ctx()->schema_guard_) {
    const ObTableSchema *table = nullptr;
    ObSEArray<AiColumnDefinition, 4> defs;
    if (OB_SUCCESS == ctx.get_sql_ctx()->schema_guard_->get_table_schema(ctdef.das_base_ctdef_.index_tid_, table)
        && nullptr != table && OB_SUCCESS == AiColumnSchema::get_definitions(*table, defs)) {
      needed = !defs.empty();
    }
  }
  return needed;
}

int AiColumnDml::after_row(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef)
{
  int ret = OB_SUCCESS;
  if (is_internal() || !ctdef.is_primary_index_) { return ret; }
  ObExecContext &ctx = op.get_exec_ctx();
  const ObTableSchema *table = nullptr;
  ObSEArray<AiColumnDefinition, 4> defs;
  OZ(ctx.get_sql_ctx()->schema_guard_->get_table_schema(ctdef.das_base_ctdef_.index_tid_, table));
  CK(nullptr != table);
  OZ(AiColumnSchema::get_definitions(*table, defs));
  if (OB_FAIL(ret) || defs.empty()) { return ret; }
  InternalGuard guard;
  ObSqlString name, old_key, new_key, request;
  const bool deleting = ctdef.new_row_.empty();
  const bool inserting = ctdef.old_row_.empty();
  OZ(table_name(ctx, *table, name));
  if (!inserting) { OZ(row_predicate(op, ctdef, *table, ctdef.old_row_, old_key)); }
  if (!deleting) { OZ(row_predicate(op, ctdef, *table, ctdef.new_row_, new_key)); }
  // One generation shared by every affected AI column on this business row.
  OZ(request.append_fmt("%lu-%ld-%lu", ctx.get_my_session()->get_server_sid(),
                        ObTimeUtility::current_time(), reinterpret_cast<uint64_t>(&op)));
  ObISQLConnectionGuard connection;
  OZ(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(ctx.get_my_session(), connection));
  ObISQLConnection *conn = connection.get_ptr();
  CK(nullptr != conn);
  if (OB_SUCC(ret) && !inserting && (deleting || old_key.string() != new_key.string())) {
    OZ(remove_tasks(*conn, table->get_table_id(), old_key.string()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && !deleting && i < defs.count(); ++i) {
    const AiColumnDefinition &def = defs.at(i);
    bool changed = inserting || old_key.string() != new_key.string();
    for (int64_t j = 0; OB_SUCC(ret) && !changed && j < def.dependency_column_ids_.count(); ++j) {
      for (int64_t k = 0; OB_SUCC(ret) && !changed && k < ctdef.column_ids_.count(); ++k) {
        if (ctdef.column_ids_.at(k) == def.dependency_column_ids_.at(j)) {
          ObDatum *old_datum = nullptr, *new_datum = nullptr;
          OZ(ctdef.old_row_.at(k)->eval(op.get_eval_ctx(), old_datum));
          OZ(ctdef.new_row_.at(k)->eval(op.get_eval_ctx(), new_datum));
          if (OB_SUCC(ret)) { changed = !ObDatum::binary_equal(*old_datum, *new_datum); }
        }
      }
    }
    if (OB_SUCC(ret) && changed) {
      ObSqlString sql;
      OZ(sql.append_fmt("UPDATE %.*s SET ", name.length(), name.ptr()));
      OZ(quote_identifier(sql, def.column_name_));
      OZ(sql.append_fmt("=NULL,`__ai_state_%lu`=0,`__ai_req_%lu`=", def.column_id_, def.column_id_));
      OZ(quote_string(sql, request.string()));
      OZ(sql.append_fmt(" WHERE %.*s", new_key.length(), new_key.ptr()));
      // This is deliberately normal SQL DML: vector and secondary indexes must
      // observe the old-result removal in the same business transaction.
      OZ(write_sql(*conn, sql));
      OZ(enqueue(*conn, *table, def, name.string(), new_key.string(), request.string()));
    }
  }
  return ret;
}

int AiColumnDml::retry(ObExecContext &ctx, ObAlterTableStmt &stmt)
{
  int ret = OB_SUCCESS;
  InternalGuard guard;
  const ObTableSchema *table = nullptr;
  AiColumnDefinition def;
  const uint64_t table_id = stmt.get_alter_table_schema().get_table_id();
  OZ(ctx.get_sql_ctx()->schema_guard_->get_table_schema(table_id, table));
  CK(nullptr != table);
  OZ(AiColumnSchema::get_definition(*table, stmt.get_ai_retry_column_id(), def));
  ObSqlString name, predicate, request;
  OZ(table_name(ctx, *table, name));
  OZ(request.append_fmt("retry-%lu-%ld", ctx.get_my_session()->get_server_sid(), ObTimeUtility::current_time()));
  // Rewrite only unquoted STATUS tokens. Quoted identifiers, string values,
  // and comments retain their ordinary SQL meanings (including `status`).
  const ObString &input = stmt.get_ai_retry_predicate();
  for (int64_t i = 0; OB_SUCC(ret) && i < input.length();) {
    const char c = input.ptr()[i];
    if (c == '\'' || c == '"' || c == '`') {
      const int64_t start = i++;
      while (i < input.length()) {
        if (input.ptr()[i] == '\\' && c != '`') { i += MIN(2, input.length() - i); }
        else if (input.ptr()[i++] == c) {
          if (i < input.length() && input.ptr()[i] == c) { ++i; }
          else { break; }
        }
      }
      OZ(predicate.append(input.ptr() + start, i - start));
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
      const int64_t start = i++;
      while (i < input.length() && (isalnum(static_cast<unsigned char>(input.ptr()[i])) || input.ptr()[i] == '_' || input.ptr()[i] == '$')) { ++i; }
      if (i - start == 6 && 0 == STRNCASECMP(input.ptr() + start, "status", 6)) {
        OZ(predicate.append_fmt("(CASE `__ai_state_%lu` WHEN 1 THEN 'READY' WHEN 2 THEN 'FAILED' ELSE 'PENDING' END)", def.column_id_));
      } else { OZ(predicate.append(input.ptr() + start, i - start)); }
    } else if (c == '/' && i + 1 < input.length() && input.ptr()[i + 1] == '*') {
      const int64_t start = i;
      i += 2;
      while (i + 1 < input.length() && !(input.ptr()[i] == '*' && input.ptr()[i + 1] == '/')) { ++i; }
      i = MIN(input.length(), i + 2);
      OZ(predicate.append(input.ptr() + start, i - start));
    } else { OZ(predicate.append(input.ptr() + i++, 1)); }
  }
  if (input.empty()) { OZ(predicate.append("1")); }
  ObISQLConnectionGuard connection;
  OZ(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(ctx.get_my_session(), connection));
  ObISQLConnection *conn = connection.get_ptr();
  CK(nullptr != conn);
  bool started = false;
  if (OB_SUCC(ret)) {
    OZ(conn->start_transaction());
    started = OB_SUCC(ret);
  }
  ObSqlString update;
  OZ(update.append_fmt("UPDATE %.*s SET ", name.length(), name.ptr()));
  OZ(quote_identifier(update, def.column_name_));
  OZ(update.append_fmt("=NULL,`__ai_state_%lu`=0,`__ai_req_%lu`=", def.column_id_, def.column_id_));
  OZ(quote_string(update, request.string()));
  OZ(update.append_fmt(" WHERE %.*s", predicate.length(), predicate.ptr()));
  OZ(write_sql(*conn, update));
  // Enumerate the rows actually locked and invalidated above, not the original
  // predicate: STATUS='READY' no longer matches after the update.
  ObSqlString select;
  OZ(select.append("SELECT CONCAT("));
  const ObRowkeyInfo &key = table->get_rowkey_info();
  for (int64_t i = 0; OB_SUCC(ret) && i < key.get_size(); ++i) {
    uint64_t cid = OB_INVALID_ID;
    OZ(key.get_column_id(i, cid));
    const ObColumnSchemaV2 *col = table->get_column_schema(cid);
    CK(nullptr != col);
    ObSqlString label;
    if (i > 0) { OZ(select.append(",")); OZ(label.append(" AND ")); }
    OZ(quote_identifier(label, col->get_column_name_str()));
    OZ(label.append(" = "));
    OZ(quote_string(select, label.string()));
    OZ(select.append(","));
    const bool number = ob_is_numeric_type(col->get_data_type());
    OZ(select.append(number ? "CAST(" : "QUOTE("));
    OZ(quote_identifier(select, col->get_column_name_str()));
    OZ(select.append(number ? " AS CHAR)" : ")"));
  }
  OZ(select.append_fmt(") AS rowkey_predicate FROM %.*s WHERE `__ai_req_%lu`=", name.length(), name.ptr(), def.column_id_));
  OZ(quote_string(select, request.string()));
  ObSEArray<ObString, 16> keys;
  {
    ObISQLClient::ReadResult result;
    OZ(conn->execute_read(select.string(), result, true));
    ObMySQLResult *rows = result.get_result();
    CK(nullptr != rows);
    while (OB_SUCC(ret)) {
      if (OB_FAIL(rows->next())) {
        if (OB_ITER_END == ret) { ret = OB_SUCCESS; break; }
      } else {
        ObString value, copy;
        OZ(rows->get_varchar("rowkey_predicate", value));
        OZ(ob_write_string(ctx.get_allocator(), value, copy));
        OZ(keys.push_back(copy));
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < keys.count(); ++i) {
    OZ(enqueue(*conn, *table, def, name.string(), keys.at(i), request.string()));
  }
  if (started) {
    if (OB_SUCC(ret)) { OZ(conn->commit()); }
    else { const int tmp_ret = conn->rollback(); UNUSED(tmp_ret); }
  }
  return ret;
}
}
}
