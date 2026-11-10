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

#include "share/schema/ai_column_schema.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/dml/ob_ai_column_dml.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_stmt_expr_visitor.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "query/session/ob_session_inner_sql.h"

namespace oceanbase { namespace sql {

// Coordinator-side preflight, deliberately independent of the optimized operator
// tree. A filter folded to FALSE or an unused subquery must not erase a read.
class AiColumnStrictRead
{
public:
  static int collect(const ObDMLStmt &stmt, share::schema::ObSchemaGetterGuard &guard,
                     ObPhysicalPlan &plan)
  {
    int ret = OB_SUCCESS;
    common::ObSEArray<ObRawExpr *, 16> exprs;
    common::ObSEArray<ObRawExpr *, 16> columns;
    ObStmtExprGetter getter;
    getter.set_relation_scope();
    getter.remove_scope(SCOPE_DML_COLUMN); // write targets are not reads
    getter.remove_scope(SCOPE_DML_CONSTRAINT);
    getter.remove_scope(SCOPE_DMLINFOS);
    if (stmt.is_explain_stmt()) {
    } else if (OB_FAIL(stmt.get_relation_exprs(exprs, getter))) {
    } else if (OB_FAIL(ObRawExprUtils::extract_column_exprs(exprs, columns))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      const ObColumnRefRawExpr &col = *static_cast<ObColumnRefRawExpr *>(columns.at(i));
      const TableItem *item = stmt.get_table_item_by_id(col.get_table_id());
      const share::schema::ObTableSchema *table = nullptr;
      if (nullptr == item || !item->is_basic_table()) {
      } else if (OB_FAIL(guard.get_table_schema(item->ref_id_, table))) {
      } else if (OB_ISNULL(table)) {
        ret = OB_TABLE_NOT_EXIST;
      } else {
        share::schema::AiColumnDefinition def;
        ret = share::schema::AiColumnSchema::get_definition(*table, col.get_column_id(), def);
        if (OB_ENTRY_NOT_EXIST == ret) {
          ret = OB_SUCCESS;
        } else if (OB_SUCC(ret)) {
          // Flat pairs avoid borrowed schema strings in cached plans.
          common::ObIArray<uint64_t> &refs = plan.get_ai_column_read_refs();
          bool found = false;
          for (int64_t j = 0; !found && j < refs.count(); j += 2) {
            found = refs.at(j) == item->ref_id_ && refs.at(j + 1) == col.get_column_id();
          }
          if (!found && OB_FAIL(refs.push_back(item->ref_id_))) {
          } else if (!found && OB_FAIL(refs.push_back(col.get_column_id()))) {
          }
        }
      }
    }
    common::ObSEArray<ObSelectStmt *, 8> children;
    if (OB_SUCC(ret) && !stmt.is_explain_stmt()) {
      if (OB_FAIL(stmt.get_child_stmts(children))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < children.count(); ++i) {
        if (OB_ISNULL(children.at(i))) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(SMART_CALL(collect(*children.at(i), guard, plan)))) {
        }
      }
    }
    return ret;
  }

  static int check(ObSQLSessionInfo &session, share::schema::ObSchemaGetterGuard &guard,
                   const ObPhysicalPlan &plan)
  {
    int ret = OB_SUCCESS;
    bool enabled = false;
    const common::ObIArray<uint64_t> &refs = plan.get_ai_column_read_refs();
    if (AiColumnDml::is_internal() || refs.empty()) {
    } else if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, enabled))) {
    } else if (enabled) {
      // Use the caller's connection/transaction, never a proxy connection with
      // a different snapshot (which would miss this transaction's own writes).
      AiColumnDml::InternalGuard internal_guard;
      query::ObSessionInnerSql sql(&session);
      for (int64_t i = 0; OB_SUCC(ret) && i < refs.count(); i += 2) {
        const share::schema::ObTableSchema *table = nullptr;
        const share::schema::ObDatabaseSchema *db = nullptr;
        share::schema::AiColumnDefinition def;
        const share::schema::ObColumnSchemaV2 *state = nullptr;
        common::ObSqlString query;
        if (OB_FAIL(guard.get_table_schema(refs.at(i), table))) {
        } else if (OB_ISNULL(table)) {
          ret = OB_TABLE_NOT_EXIST;
        } else if (OB_FAIL(guard.get_database_schema(table->get_database_id(), db))) {
        } else if (OB_ISNULL(db)) {
          ret = OB_ERR_BAD_DATABASE;
        } else if (OB_FAIL(share::schema::AiColumnSchema::get_definition(*table, refs.at(i + 1), def))) {
        } else if (OB_ISNULL(state = table->get_column_schema(def.state_column_id_))) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(query.append("SELECT 1 FROM "))) {
        } else if (OB_FAIL(quote_identifier(db->get_database_name_str(), query))) {
        } else if (OB_FAIL(query.append("."))) {
        } else if (OB_FAIL(quote_identifier(table->get_table_name_str(), query))) {
        } else if (OB_FAIL(query.append(" WHERE "))) {
        } else if (OB_FAIL(quote_identifier(state->get_column_name_str(), query))) {
        } else if (OB_FAIL(query.append(" IS NULL OR "))) {
        } else if (OB_FAIL(quote_identifier(state->get_column_name_str(), query))) {
        } else if (OB_FAIL(query.append(" <> 1 LIMIT 1"))) {
        } else {
          common::ObISQLClient::ReadResult result;
          if (OB_FAIL(sql.execute_read(query, result))) {
          } else if (OB_ISNULL(result.get_result())) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(result.get_result()->next())) {
            if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
          } else {
            ret = OB_ERR_AI_COLUMN_UNFINISHED;
            LOG_USER_ERROR(OB_ERR_AI_COLUMN_UNFINISHED, refs.at(i), refs.at(i + 1));
          }
        }
      }
    }
    return ret;
  }
private:
  static int quote_identifier(const common::ObString &name, common::ObSqlString &out)
  {
    int ret = out.append("`");
    for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
      if ('`' == name.ptr()[i] && OB_FAIL(out.append("`"))) {
      } else if (OB_FAIL(out.append(name.ptr() + i, 1))) {
      }
    }
    if (OB_SUCC(ret)) { ret = out.append("`"); }
    return ret;
  }
};
}} // namespace oceanbase::sql
