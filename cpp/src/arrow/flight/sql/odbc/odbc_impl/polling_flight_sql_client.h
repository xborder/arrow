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

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "arrow/flight/client.h"
#include "arrow/flight/sql/client.h"
#include "arrow/flight/sql/poll_info_internal.h"

namespace arrow::flight::sql::odbc {

/// Flight SQL client used by one physical ODBC connection.
///
/// It keeps PollFlightInfo capability failures scoped to the exact Flight SQL command
/// family on that connection.  No state is shared across physical connections.
class PollingFlightSqlClient : public FlightSqlClient {
 public:
  PollingFlightSqlClient(std::shared_ptr<FlightClient> client, bool polling_enabled);

  /// Test seam; production connections use the FlightClient constructor above.
  PollingFlightSqlClient(std::unique_ptr<internal::PollInfoRpcClient> rpc_client,
                         bool polling_enabled);

  arrow::Result<std::unique_ptr<FlightInfo>> GetFlightInfo(
      const FlightCallOptions& options, const FlightDescriptor& descriptor) override;

  /// Transfer the progressive operation created by the most recent FlightInfo
  /// request on this thread to the result reader.
  std::shared_ptr<internal::ProgressivePollInfoOperation> TakeProgressiveOperation();

  bool IsUnsupportedForTesting(const std::string& family) const;

 private:
  bool IsUnsupported(const std::string& family) const;
  void MarkUnsupported(const std::string& family);
  void SetProgressiveOperation(
      std::shared_ptr<internal::ProgressivePollInfoOperation> operation);

  std::shared_ptr<FlightClient> flight_client_;
  std::unique_ptr<internal::PollInfoRpcClient> rpc_client_;
  bool polling_enabled_;
  mutable std::mutex mutex_;
  std::unordered_set<std::string> unsupported_families_;
  std::unordered_map<std::thread::id,
                     std::shared_ptr<internal::ProgressivePollInfoOperation>>
      progressive_operations_;
};

}  // namespace arrow::flight::sql::odbc
