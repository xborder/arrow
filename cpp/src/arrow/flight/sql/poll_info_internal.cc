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

#include "arrow/flight/sql/poll_info_internal.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

#include <google/protobuf/any.pb.h>

#include "arrow/status.h"
#include "arrow/util/cancel.h"
#include "arrow/util/logging.h"

namespace arrow::flight::sql::internal {
namespace {

class OperationDeadline {
 public:
  explicit OperationDeadline(const FlightCallOptions& options) {
    if (options.timeout.count() >= 0) {
      deadline_ = std::chrono::steady_clock::now() +
                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      options.timeout);
    }
  }

  arrow::Result<FlightCallOptions> Remaining(
      const FlightCallOptions& original) const {
    FlightCallOptions options = original;
    if (!deadline_) {
      return options;
    }
    const auto remaining = *deadline_ - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      return Status::IOError("Flight SQL PollInfo operation timed out");
    }
    options.timeout =
        std::chrono::duration_cast<TimeoutDuration>(remaining);
    return options;
  }

 private:
  std::optional<std::chrono::steady_clock::time_point> deadline_;
};

void TryCancel(PollInfoRpcClient* client, const FlightCallOptions& original_options,
               const FlightInfo* latest_info) {
  if (latest_info == nullptr) {
    return;
  }
  FlightCallOptions options = original_options;
  options.stop_token = StopToken::Unstoppable();
  options.timeout = TimeoutDuration{1.0};
  CancelFlightInfoRequest request{std::make_unique<FlightInfo>(*latest_info)};
  auto result = client->CancelFlightInfo(options, request);
  if (!result.ok()) {
    ARROW_LOG(DEBUG) << "Best-effort CancelFlightInfo failed: "
                     << result.status().ToString();
  }
}

bool IsFlightTimedOut(const Status& status) {
  const auto detail = FlightStatusDetail::UnwrapStatus(status);
  return detail != nullptr && detail->code() == FlightStatusCode::TimedOut;
}

}  // namespace

arrow::Result<std::unique_ptr<PollInfo>>
FlightClientPollInfoRpcClient::PollFlightInfo(
    const FlightCallOptions& options, const FlightDescriptor& descriptor) {
  return client_->PollFlightInfo(options, descriptor);
}

arrow::Result<std::unique_ptr<FlightInfo>>
FlightClientPollInfoRpcClient::GetFlightInfo(
    const FlightCallOptions& options, const FlightDescriptor& descriptor) {
  return client_->GetFlightInfo(options, descriptor);
}

arrow::Result<CancelFlightInfoResult>
FlightClientPollInfoRpcClient::CancelFlightInfo(
    const FlightCallOptions& options, const CancelFlightInfoRequest& request) {
  return client_->CancelFlightInfo(options, request);
}

arrow::Result<std::unique_ptr<FlightInfo>> PollFlightInfoUntilComplete(
    PollInfoRpcClient* client, const FlightCallOptions& options,
    const FlightDescriptor& original_descriptor, bool* initial_poll_unimplemented) {
  if (initial_poll_unimplemented != nullptr) {
    *initial_poll_unimplemented = false;
  }

  OperationDeadline deadline(options);
  FlightDescriptor descriptor = original_descriptor;
  std::unique_ptr<FlightInfo> latest_info;
  bool initial = true;

  while (true) {
    Status stop_status = options.stop_token.Poll();
    if (!stop_status.ok()) {
      TryCancel(client, options, latest_info.get());
      return stop_status;
    }

    auto remaining = deadline.Remaining(options);
    if (!remaining.ok()) {
      TryCancel(client, options, latest_info.get());
      return remaining.status();
    }
    FlightCallOptions call_options = std::move(*remaining);
    auto poll_result = client->PollFlightInfo(call_options, descriptor);
    if (!poll_result.ok()) {
      if (initial && poll_result.status().IsNotImplemented()) {
        if (initial_poll_unimplemented != nullptr) {
          *initial_poll_unimplemented = true;
        }
        ARROW_ASSIGN_OR_RAISE(call_options, deadline.Remaining(options));
        return client->GetFlightInfo(call_options, original_descriptor);
      }
      if (options.stop_token.IsStopRequested()) {
        TryCancel(client, options, latest_info.get());
        return options.stop_token.Poll();
      }
      if (IsFlightTimedOut(poll_result.status())) {
        TryCancel(client, options, latest_info.get());
      }
      return poll_result.status();
    }

    std::unique_ptr<PollInfo> poll_info = std::move(*poll_result);
    if (!poll_info) {
      return Status::Invalid("PollFlightInfo returned a null PollInfo");
    }
    if (poll_info->info) {
      latest_info = std::move(poll_info->info);
    }
    if (!poll_info->descriptor) {
      if (!latest_info) {
        return Status::Invalid(
            "Final PollFlightInfo response did not contain cumulative FlightInfo");
      }
      return latest_info;
    }

    descriptor = std::move(*poll_info->descriptor);
    initial = false;
  }
}

std::string GetFlightSqlCommandFamily(const FlightDescriptor& descriptor) {
  if (descriptor.type != FlightDescriptor::CMD) {
    return "flight-descriptor:path";
  }
  google::protobuf::Any command;
  if (!command.ParseFromString(descriptor.cmd) || command.type_url().empty()) {
    return "flight-descriptor:opaque-command";
  }
  return command.type_url();
}

}  // namespace arrow::flight::sql::internal
