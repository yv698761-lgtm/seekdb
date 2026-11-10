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

#include "share/ob_thread_pool.h"
#include <string>
#include <vector>

namespace oceanbase
{
namespace observer
{
// Durable tasks are visible to this service only after the inserting business
// transaction commits. Leases fence interrupted executions and permit recovery.
class AiColumnWorker final : public share::ObThreadPool
{
public:
  AiColumnWorker();
  ~AiColumnWorker();
  int init();
  void run1() override;
private:
  struct Task
  {
    uint64_t table_id = 0;
    uint64_t column_id = 0;
    uint64_t rowkey_hash = 0;
    int64_t definition_version = 0;
    int64_t retry_count = 0;
    int64_t state = 0;
    std::string rowkey;
    std::string request_id;
    std::string prompt;
    std::string result;
    std::string error;
    bool result_null = false;
  };
  int poll();
  int claim_group(const Task &candidate, const std::string &token, bool &claimed);
  int read_group(const Task &candidate, const std::string &token, std::vector<Task> &tasks);
  int execute(Task &task, const std::string &token);
  int publish(const Task &candidate, const std::string &token);
  bool inited_;
};
}
}
