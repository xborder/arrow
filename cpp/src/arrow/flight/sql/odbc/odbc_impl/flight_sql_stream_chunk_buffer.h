// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <mutex>

#include "arrow/flight/client.h"
#include "arrow/flight/sql/client.h"
#include "arrow/flight/sql/odbc/odbc_impl/blocking_queue.h"
#include "arrow/flight/sql/poll_info_internal.h"

namespace arrow::flight::sql::odbc {

class FlightStreamChunkBuffer {
  FlightSqlClient& flight_sql_client_;
  FlightClientOptions client_options_;
  FlightCallOptions call_options_;
  std::shared_ptr<internal::ProgressivePollInfoOperation> poll_info_operation_;
  size_t published_endpoint_count_ = 0;
  std::mutex state_mutex_;
  bool closed_ = false;
  BlockingQueue<
      std::pair<arrow::Result<FlightStreamChunk>, std::shared_ptr<FlightSqlClient>>>
      queue_;

  void AddEndpoints(const FlightInfo& flight_info);

 public:
  FlightStreamChunkBuffer(
      FlightSqlClient& flight_sql_client, const FlightClientOptions& client_options,
      const FlightCallOptions& call_options,
      const std::shared_ptr<FlightInfo>& flight_info, size_t queue_capacity = 5,
      std::shared_ptr<internal::ProgressivePollInfoOperation> poll_info_operation =
          nullptr);

  ~FlightStreamChunkBuffer();

  void Close();

  bool GetNext(FlightStreamChunk* chunk);
};

}  // namespace arrow::flight::sql::odbc
