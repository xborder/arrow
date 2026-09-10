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

#include "arrow/flight/sql/odbc/odbc_impl/polling_flight_sql_client.h"

#include <utility>

namespace arrow::flight::sql::odbc {

PollingFlightSqlClient::PollingFlightSqlClient(std::shared_ptr<FlightClient> client,
                                               bool polling_enabled)
    : FlightSqlClient(client),
      flight_client_(std::move(client)),
      rpc_client_(
          std::make_unique<internal::FlightClientPollInfoRpcClient>(flight_client_.get())),
      polling_enabled_(polling_enabled) {}

PollingFlightSqlClient::PollingFlightSqlClient(
    std::unique_ptr<internal::PollInfoRpcClient> rpc_client, bool polling_enabled)
    : FlightSqlClient(std::shared_ptr<FlightClient>{}),
      rpc_client_(std::move(rpc_client)),
      polling_enabled_(polling_enabled) {}

arrow::Result<std::unique_ptr<FlightInfo>> PollingFlightSqlClient::GetFlightInfo(
    const FlightCallOptions& options, const FlightDescriptor& descriptor) {
  const std::string family = internal::GetFlightSqlCommandFamily(descriptor);
  if (!polling_enabled_ || IsUnsupported(family)) {
    return rpc_client_->GetFlightInfo(options, descriptor);
  }

  bool initial_poll_unimplemented = false;
  auto result = internal::PollFlightInfoUntilComplete(
      rpc_client_.get(), options, descriptor, &initial_poll_unimplemented);
  if (initial_poll_unimplemented) {
    MarkUnsupported(family);
  }
  return result;
}

bool PollingFlightSqlClient::IsUnsupported(const std::string& family) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return unsupported_families_.count(family) != 0;
}

void PollingFlightSqlClient::MarkUnsupported(const std::string& family) {
  std::lock_guard<std::mutex> lock(mutex_);
  unsupported_families_.insert(family);
}

bool PollingFlightSqlClient::IsUnsupportedForTesting(
    const std::string& family) const {
  return IsUnsupported(family);
}

}  // namespace arrow::flight::sql::odbc
