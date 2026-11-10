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

#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{
class ObTableModifyOp;
struct ObDMLBaseCtDef;
class ObExecContext;
class ObAlterTableStmt;

// T1 only: all SQL uses the caller's external session/transaction. Model calls
// belong exclusively to the committed-task worker, never to these hooks.
class AiColumnDml
{
public:
  class InternalGuard
  {
  public:
    InternalGuard();
    ~InternalGuard();
  private:
    InternalGuard(const InternalGuard &) = delete;
    InternalGuard &operator=(const InternalGuard &) = delete;
  };
  static bool is_internal();
  static bool needs_after_row(ObExecContext &ctx, const ObDMLBaseCtDef &ctdef);
  static int after_row(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef);
  static int retry(ObExecContext &ctx, ObAlterTableStmt &stmt);
};
}
}
