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
#include <atomic>
#include <chrono>
#include <mutex>
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

  arrow::Result<FlightCallOptions> Remaining(const FlightCallOptions& original) const {
    FlightCallOptions options = original;
    if (!deadline_) {
      return options;
    }
    const auto remaining = *deadline_ - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      return Status::IOError("Flight SQL PollInfo operation timed out");
    }
    options.timeout = std::chrono::duration_cast<TimeoutDuration>(remaining);
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

arrow::Result<std::unique_ptr<PollInfo>> FlightClientPollInfoRpcClient::PollFlightInfo(
    const FlightCallOptions& options, const FlightDescriptor& descriptor) {
  return client_->PollFlightInfo(options, descriptor);
}

arrow::Result<std::unique_ptr<FlightInfo>> FlightClientPollInfoRpcClient::GetFlightInfo(
    const FlightCallOptions& options, const FlightDescriptor& descriptor) {
  return client_->GetFlightInfo(options, descriptor);
}

arrow::Result<CancelFlightInfoResult> FlightClientPollInfoRpcClient::CancelFlightInfo(
    const FlightCallOptions& options, const CancelFlightInfoRequest& request) {
  return client_->CancelFlightInfo(options, request);
}

class ProgressivePollInfoOperation::Impl {
 public:
  Impl(PollInfoRpcClient* client, const FlightCallOptions& options,
       const FlightDescriptor& original_descriptor)
      : client_(client),
        options_(options),
        original_descriptor_(original_descriptor),
        descriptor_(original_descriptor),
        deadline_(options) {}

  arrow::Result<std::unique_ptr<FlightInfo>> Start(bool* initial_poll_unimplemented) {
    if (initial_poll_unimplemented != nullptr) {
      *initial_poll_unimplemented = false;
    }
    return PollUntilAvailable(0, initial_poll_unimplemented);
  }

  arrow::Result<std::unique_ptr<FlightInfo>> PollNextAvailable() {
    size_t previous_endpoint_count = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (latest_info_ != nullptr) {
        previous_endpoint_count = latest_info_->endpoints().size();
      }
    }
    return PollUntilAvailable(previous_endpoint_count, nullptr);
  }

  void Cancel() {
    if (!complete_) {
      TryCancelOnce();
    }
  }

  bool is_complete() const { return complete_.load(); }

 private:
  arrow::Result<std::unique_ptr<FlightInfo>> PollUntilAvailable(
      size_t previous_endpoint_count, bool* initial_poll_unimplemented) {
    while (true) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (complete_) {
          if (latest_info_ == nullptr) {
            return Status::Invalid(
                "PollInfo operation completed without cumulative FlightInfo");
          }
          return std::make_unique<FlightInfo>(*latest_info_);
        }
      }

      Status stop_status = options_.stop_token.Poll();
      if (!stop_status.ok()) {
        complete_ = true;
        TryCancelOnce();
        return stop_status;
      }

      auto remaining = deadline_.Remaining(options_);
      if (!remaining.ok()) {
        complete_ = true;
        TryCancelOnce();
        return remaining.status();
      }

      auto poll_result = client_->PollFlightInfo(*remaining, descriptor_);
      if (!poll_result.ok()) {
        if (initial_ && poll_result.status().IsNotImplemented()) {
          if (initial_poll_unimplemented != nullptr) {
            *initial_poll_unimplemented = true;
          }
          ARROW_ASSIGN_OR_RAISE(auto get_options, deadline_.Remaining(options_));
          auto get_result = client_->GetFlightInfo(get_options, original_descriptor_);
          complete_ = true;
          if (!get_result.ok()) {
            return get_result.status();
          }
          std::lock_guard<std::mutex> lock(mutex_);
          latest_info_ = std::move(*get_result);
          return std::make_unique<FlightInfo>(*latest_info_);
        }

        complete_ = true;
        if (options_.stop_token.IsStopRequested()) {
          TryCancelOnce();
          return options_.stop_token.Poll();
        }
        if (IsFlightTimedOut(poll_result.status())) {
          TryCancelOnce();
        }
        return poll_result.status();
      }

      std::unique_ptr<PollInfo> poll_info = std::move(*poll_result);
      if (poll_info == nullptr) {
        complete_ = true;
        return Status::Invalid("PollFlightInfo returned a null PollInfo");
      }

      bool has_new_endpoint = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (poll_info->info != nullptr) {
          const Status validation = ValidateAppendOnly(*poll_info->info);
          if (!validation.ok()) {
            complete_ = true;
            return validation;
          }
          latest_info_ = std::move(poll_info->info);
          has_new_endpoint = latest_info_->endpoints().size() > previous_endpoint_count;
        }
        if (poll_info->descriptor) {
          descriptor_ = std::move(*poll_info->descriptor);
        } else {
          complete_ = true;
        }
        initial_ = false;

        if (complete_ && latest_info_ == nullptr) {
          return Status::Invalid(
              "Final PollFlightInfo response did not contain cumulative FlightInfo");
        }
        if (has_new_endpoint || complete_) {
          return std::make_unique<FlightInfo>(*latest_info_);
        }
      }
    }
  }

  Status ValidateAppendOnly(const FlightInfo& current) const {
    if (latest_info_ == nullptr) {
      return Status::OK();
    }
    const auto& previous_endpoints = latest_info_->endpoints();
    const auto& current_endpoints = current.endpoints();
    if (current_endpoints.size() < previous_endpoints.size()) {
      return Status::Invalid("PollInfo removed previously published endpoints");
    }
    for (size_t index = 0; index < previous_endpoints.size(); ++index) {
      if (previous_endpoints[index] != current_endpoints[index]) {
        return Status::Invalid("PollInfo mutated previously published endpoint ", index);
      }
    }
    return Status::OK();
  }

  void TryCancelOnce() {
    std::unique_ptr<FlightInfo> latest_info;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (latest_info_ == nullptr || cancel_attempted_.exchange(true)) {
        return;
      }
      latest_info = std::make_unique<FlightInfo>(*latest_info_);
    }
    TryCancel(client_, options_, latest_info.get());
  }

  PollInfoRpcClient* client_;
  FlightCallOptions options_;
  FlightDescriptor original_descriptor_;
  FlightDescriptor descriptor_;
  OperationDeadline deadline_;
  mutable std::mutex mutex_;
  std::unique_ptr<FlightInfo> latest_info_;
  std::atomic<bool> complete_{false};
  std::atomic<bool> cancel_attempted_{false};
  bool initial_ = true;
};

ProgressivePollInfoOperation::ProgressivePollInfoOperation(
    PollInfoRpcClient* client, const FlightCallOptions& options,
    const FlightDescriptor& original_descriptor)
    : impl_(std::make_unique<Impl>(client, options, original_descriptor)) {}

ProgressivePollInfoOperation::~ProgressivePollInfoOperation() = default;

arrow::Result<std::unique_ptr<FlightInfo>> ProgressivePollInfoOperation::Start(
    bool* initial_poll_unimplemented) {
  return impl_->Start(initial_poll_unimplemented);
}

arrow::Result<std::unique_ptr<FlightInfo>>
ProgressivePollInfoOperation::PollNextAvailable() {
  return impl_->PollNextAvailable();
}

void ProgressivePollInfoOperation::Cancel() { impl_->Cancel(); }

bool ProgressivePollInfoOperation::is_complete() const { return impl_->is_complete(); }

arrow::Result<std::unique_ptr<FlightInfo>> PollFlightInfoUntilComplete(
    PollInfoRpcClient* client, const FlightCallOptions& options,
    const FlightDescriptor& original_descriptor, bool* initial_poll_unimplemented) {
  auto operation = std::make_shared<ProgressivePollInfoOperation>(client, options,
                                                                  original_descriptor);
  ARROW_ASSIGN_OR_RAISE(auto latest, operation->Start(initial_poll_unimplemented));
  while (!operation->is_complete()) {
    ARROW_ASSIGN_OR_RAISE(latest, operation->PollNextAvailable());
  }
  return latest;
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
