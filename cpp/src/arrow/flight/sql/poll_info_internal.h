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
#include <string>

#include "arrow/flight/client.h"
#include "arrow/flight/sql/visibility.h"
#include "arrow/flight/types.h"
#include "arrow/result.h"

namespace arrow::flight::sql::internal {

/// Internal seam for exercising the synchronous PollInfo orchestration without a
/// transport.  Production callers should use FlightClientPollInfoRpcClient.
class ARROW_FLIGHT_SQL_EXPORT PollInfoRpcClient {
 public:
  virtual ~PollInfoRpcClient() = default;

  virtual arrow::Result<std::unique_ptr<PollInfo>> PollFlightInfo(
      const FlightCallOptions& options, const FlightDescriptor& descriptor) = 0;
  virtual arrow::Result<std::unique_ptr<FlightInfo>> GetFlightInfo(
      const FlightCallOptions& options, const FlightDescriptor& descriptor) = 0;
  virtual arrow::Result<CancelFlightInfoResult> CancelFlightInfo(
      const FlightCallOptions& options, const CancelFlightInfoRequest& request) = 0;
};

class ARROW_FLIGHT_SQL_EXPORT FlightClientPollInfoRpcClient final
    : public PollInfoRpcClient {
 public:
  explicit FlightClientPollInfoRpcClient(FlightClient* client) : client_(client) {}

  arrow::Result<std::unique_ptr<PollInfo>> PollFlightInfo(
      const FlightCallOptions& options, const FlightDescriptor& descriptor) override;
  arrow::Result<std::unique_ptr<FlightInfo>> GetFlightInfo(
      const FlightCallOptions& options, const FlightDescriptor& descriptor) override;
  arrow::Result<CancelFlightInfoResult> CancelFlightInfo(
      const FlightCallOptions& options,
      const CancelFlightInfoRequest& request) override;

 private:
  FlightClient* client_;
};

/// Poll until completion and return only the final cumulative FlightInfo.
///
/// GetFlightInfo is used only when the initial PollFlightInfo returns
/// NotImplemented.  If initial_poll_unimplemented is non-null, it is set as soon as
/// that response is observed, even if the subsequent fallback fails.
ARROW_FLIGHT_SQL_EXPORT
arrow::Result<std::unique_ptr<FlightInfo>> PollFlightInfoUntilComplete(
    PollInfoRpcClient* client, const FlightCallOptions& options,
    const FlightDescriptor& original_descriptor,
    bool* initial_poll_unimplemented = nullptr);

/// Return a stable cache key for the command encoded in a Flight SQL descriptor.
/// Flight SQL protobuf Any type URLs are used so unsupported metadata command types
/// do not disable polling for unrelated command families.
ARROW_FLIGHT_SQL_EXPORT
std::string GetFlightSqlCommandFamily(const FlightDescriptor& descriptor);

}  // namespace arrow::flight::sql::internal
