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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <google/protobuf/any.pb.h>

#include "arrow/flight/sql/poll_info_internal.h"
#include "arrow/testing/gtest_util.h"
#include "arrow/type.h"
#include "arrow/util/cancel.h"

namespace arrow::flight::sql::odbc {
namespace {

std::unique_ptr<FlightInfo> MakeInfo(const FlightDescriptor& descriptor,
                                     const std::string& ticket) {
  auto schema = arrow::schema({arrow::field("value", arrow::int64())});
  std::vector<FlightEndpoint> endpoints = {
      FlightEndpoint{Ticket{ticket}, {}, std::nullopt, ""}};
  return std::make_unique<FlightInfo>(
      FlightInfo::Make(*schema, descriptor, endpoints, -1, -1, false).ValueOrDie());
}

std::unique_ptr<PollInfo> MakePoll(const FlightDescriptor& request,
                                   const std::string& ticket,
                                   std::optional<FlightDescriptor> continuation) {
  return std::make_unique<PollInfo>(MakeInfo(request, ticket),
                                    std::move(continuation), std::nullopt,
                                    std::nullopt);
}

FlightDescriptor CommandDescriptor(const std::string& type_name) {
  google::protobuf::Any command;
  command.set_type_url("type.googleapis.com/" + type_name);
  return FlightDescriptor::Command(command.SerializeAsString());
}

struct QueuedPoll {
  Status status;
  std::unique_ptr<PollInfo> info;
  std::chrono::milliseconds delay{0};
};

class FakePollInfoRpcClient : public internal::PollInfoRpcClient {
 public:
  arrow::Result<std::unique_ptr<PollInfo>> PollFlightInfo(
      const FlightCallOptions& options, const FlightDescriptor& descriptor) override {
    poll_descriptors.push_back(descriptor);
    poll_timeouts.push_back(options.timeout.count());
    const size_t call = poll_count++;
    if (on_poll) {
      on_poll(call, options);
    }
    if (polls.empty()) {
      return Status::Invalid("No queued poll response");
    }
    QueuedPoll response = std::move(polls.front());
    polls.pop_front();
    if (response.delay.count() > 0) {
      std::this_thread::sleep_for(response.delay);
    }
    if (!response.status.ok()) {
      return response.status;
    }
    return std::move(response.info);
  }

  arrow::Result<std::unique_ptr<FlightInfo>> GetFlightInfo(
      const FlightCallOptions& options, const FlightDescriptor& descriptor) override {
    ++get_count;
    get_descriptors.push_back(descriptor);
    get_timeouts.push_back(options.timeout.count());
    return MakeInfo(descriptor, "get");
  }

  arrow::Result<CancelFlightInfoResult> CancelFlightInfo(
      const FlightCallOptions&, const CancelFlightInfoRequest& request) override {
    ++cancel_count;
    if (request.info && !request.info->endpoints().empty()) {
      cancelled_ticket = request.info->endpoints()[0].ticket.ticket;
    }
    return CancelFlightInfoResult{CancelStatus::kCancelled};
  }

  std::deque<QueuedPoll> polls;
  std::function<void(size_t, const FlightCallOptions&)> on_poll;
  std::vector<FlightDescriptor> poll_descriptors;
  std::vector<FlightDescriptor> get_descriptors;
  std::vector<double> poll_timeouts;
  std::vector<double> get_timeouts;
  size_t poll_count = 0;
  size_t get_count = 0;
  size_t cancel_count = 0;
  std::string cancelled_ticket;
};

TEST(PollInfoOrchestrationTest, ImmediateReturnsFinalCumulativeInfo) {
  FakePollInfoRpcClient client;
  const auto original = CommandDescriptor("CommandStatementQuery");
  client.polls.push_back({Status::OK(), MakePoll(original, "final", std::nullopt)});

  ASSERT_OK_AND_ASSIGN(auto result,
                       internal::PollFlightInfoUntilComplete(&client, {}, original));
  ASSERT_EQ(1U, client.poll_descriptors.size());
  EXPECT_TRUE(client.poll_descriptors[0].Equals(original));
  EXPECT_EQ(0U, client.get_count);
  ASSERT_EQ(1U, result->endpoints().size());
  EXPECT_EQ("final", result->endpoints()[0].ticket.ticket);
}

TEST(PollInfoOrchestrationTest, DrainsContinuationsAndIgnoresPartialInfo) {
  FakePollInfoRpcClient client;
  const auto original = CommandDescriptor("CommandStatementQuery");
  const auto first = FlightDescriptor::Command("continuation-1");
  const auto second = FlightDescriptor::Command("continuation-2");
  client.polls.push_back({Status::OK(), MakePoll(original, "partial-1", first)});
  client.polls.push_back({Status::OK(), MakePoll(first, "partial-2", second)});
  client.polls.push_back({Status::OK(), MakePoll(second, "final", std::nullopt)});

  ASSERT_OK_AND_ASSIGN(auto result,
                       internal::PollFlightInfoUntilComplete(&client, {}, original));
  ASSERT_EQ(3U, client.poll_descriptors.size());
  EXPECT_TRUE(client.poll_descriptors[0].Equals(original));
  EXPECT_TRUE(client.poll_descriptors[1].Equals(first));
  EXPECT_TRUE(client.poll_descriptors[2].Equals(second));
  EXPECT_EQ(0U, client.get_count);
  EXPECT_EQ("final", result->endpoints()[0].ticket.ticket);
}

TEST(PollInfoOrchestrationTest, InitialUnimplementedFallsBackExactlyOnce) {
  FakePollInfoRpcClient client;
  const auto original = CommandDescriptor("CommandStatementQuery");
  client.polls.push_back({Status::NotImplemented("poll unsupported"), nullptr});
  bool unsupported = false;

  ASSERT_OK_AND_ASSIGN(auto result, internal::PollFlightInfoUntilComplete(
                                        &client, {}, original, &unsupported));
  EXPECT_TRUE(unsupported);
  EXPECT_EQ(1U, client.poll_count);
  EXPECT_EQ(1U, client.get_count);
  EXPECT_TRUE(client.get_descriptors[0].Equals(original));
  EXPECT_EQ("get", result->endpoints()[0].ticket.ticket);
}

TEST(PollInfoOrchestrationTest, NonInitialUnimplementedDoesNotFallback) {
  FakePollInfoRpcClient client;
  const auto original = CommandDescriptor("CommandStatementQuery");
  const auto continuation = FlightDescriptor::Command("continuation");
  client.polls.push_back(
      {Status::OK(), MakePoll(original, "partial", continuation)});
  client.polls.push_back({Status::NotImplemented("expired continuation"), nullptr});

  auto result = internal::PollFlightInfoUntilComplete(&client, {}, original);
  ASSERT_RAISES(NotImplemented, result.status());
  EXPECT_EQ(0U, client.get_count);
}

TEST(PollInfoOrchestrationTest, AmbiguousFailureDoesNotFallback) {
  FakePollInfoRpcClient client;
  const auto original = CommandDescriptor("CommandStatementQuery");
  client.polls.push_back({Status::IOError("UNAVAILABLE"), nullptr});

  auto result = internal::PollFlightInfoUntilComplete(&client, {}, original);
  ASSERT_RAISES(IOError, result.status());
  EXPECT_EQ(1U, client.poll_count);
  EXPECT_EQ(0U, client.get_count);
}

TEST(PollInfoOrchestrationTest, OneDeadlineShrinksAcrossContinuations) {
  FakePollInfoRpcClient client;
  const auto original = CommandDescriptor("CommandStatementQuery");
  const auto first = FlightDescriptor::Command("continuation-1");
  const auto second = FlightDescriptor::Command("continuation-2");
  client.polls.push_back(
      {Status::OK(), MakePoll(original, "partial-1", first),
       std::chrono::milliseconds(20)});
  client.polls.push_back(
      {Status::OK(), MakePoll(first, "partial-2", second),
       std::chrono::milliseconds(20)});
  client.polls.push_back({Status::OK(), MakePoll(second, "final", std::nullopt)});
  FlightCallOptions options;
  options.timeout = TimeoutDuration{0.25};

  ASSERT_OK(internal::PollFlightInfoUntilComplete(&client, options, original));
  ASSERT_EQ(3U, client.poll_timeouts.size());
  EXPECT_GT(client.poll_timeouts[0], client.poll_timeouts[1]);
  EXPECT_GT(client.poll_timeouts[1], client.poll_timeouts[2]);
}

TEST(PollInfoOrchestrationTest, DeadlineDoesNotResetBetweenPolls) {
  FakePollInfoRpcClient client;
  const auto original = CommandDescriptor("CommandStatementQuery");
  const auto continuation = FlightDescriptor::Command("continuation");
  client.polls.push_back(
      {Status::OK(), MakePoll(original, "partial", continuation),
       std::chrono::milliseconds(30)});
  client.polls.push_back(
      {Status::OK(), MakePoll(continuation, "wrong", std::nullopt)});
  FlightCallOptions options;
  options.timeout = TimeoutDuration{0.01};

  auto result = internal::PollFlightInfoUntilComplete(&client, options, original);
  EXPECT_RAISES_WITH_MESSAGE_THAT(IOError, ::testing::HasSubstr("timed out"),
                                  result.status());
  EXPECT_EQ(1U, client.poll_count);
  EXPECT_EQ(0U, client.get_count);
  EXPECT_EQ(1U, client.cancel_count);
  EXPECT_EQ("partial", client.cancelled_ticket);
}

TEST(PollInfoOrchestrationTest,
     TransportTimeoutAfterPartialInfoAttemptsCancellationOnce) {
  FakePollInfoRpcClient client;
  const auto original = CommandDescriptor("CommandStatementQuery");
  const auto continuation = FlightDescriptor::Command("continuation");
  client.polls.push_back(
      {Status::OK(), MakePoll(original, "partial", continuation)});
  client.polls.push_back(
      {MakeFlightError(FlightStatusCode::TimedOut, "transport deadline"), nullptr});

  auto result = internal::PollFlightInfoUntilComplete(&client, {}, original);
  ASSERT_RAISES(IOError, result.status());
  EXPECT_EQ(2U, client.poll_count);
  EXPECT_EQ(0U, client.get_count);
  EXPECT_EQ(1U, client.cancel_count);
  EXPECT_EQ("partial", client.cancelled_ticket);
}

TEST(PollInfoOrchestrationTest, CancellationAttemptsCancelFlightInfoWithLatestState) {
  auto stop_source = std::make_shared<StopSource>();
  FakePollInfoRpcClient client;
  const auto original = CommandDescriptor("CommandStatementQuery");
  const auto continuation = FlightDescriptor::Command("continuation");
  client.polls.push_back(
      {Status::OK(), MakePoll(original, "cancellable", continuation)});
  client.polls.push_back({Status::Cancelled("active poll cancelled"), nullptr});
  client.on_poll = [stop_source](size_t call, const FlightCallOptions&) {
    if (call == 1) {
      stop_source->RequestStop(Status::Cancelled("SQLCancel"));
    }
  };
  FlightCallOptions options;
  options.stop_token = stop_source->token();

  auto result = internal::PollFlightInfoUntilComplete(&client, options, original);
  EXPECT_RAISES_WITH_MESSAGE_THAT(Cancelled, ::testing::HasSubstr("SQLCancel"),
                                  result.status());
  EXPECT_EQ(1U, client.cancel_count);
  EXPECT_EQ("cancellable", client.cancelled_ticket);
  EXPECT_EQ(0U, client.get_count);
}

TEST(PollingFlightSqlClientTest, CachesUnsupportedByPhysicalClientAndFamily) {
  auto rpc_client = std::make_unique<FakePollInfoRpcClient>();
  auto* fake = rpc_client.get();
  const auto direct = CommandDescriptor("CommandStatementQuery");
  const auto metadata = CommandDescriptor("CommandGetTables");
  fake->polls.push_back({Status::NotImplemented("direct unsupported"), nullptr});
  fake->polls.push_back(
      {Status::OK(), MakePoll(metadata, "metadata-final", std::nullopt)});
  PollingFlightSqlClient client(std::move(rpc_client), true);

  ASSERT_OK(client.GetFlightInfo({}, direct));
  ASSERT_OK(client.GetFlightInfo({}, direct));
  ASSERT_OK(client.GetFlightInfo({}, metadata));

  EXPECT_EQ(2U, fake->poll_count);
  EXPECT_EQ(2U, fake->get_count);
  EXPECT_TRUE(client.IsUnsupportedForTesting(
      internal::GetFlightSqlCommandFamily(direct)));
  EXPECT_FALSE(client.IsUnsupportedForTesting(
      internal::GetFlightSqlCommandFamily(metadata)));
}

TEST(PollingFlightSqlClientTest, ConnectionOptOutSkipsPolling) {
  auto rpc_client = std::make_unique<FakePollInfoRpcClient>();
  auto* fake = rpc_client.get();
  PollingFlightSqlClient client(std::move(rpc_client), false);

  ASSERT_OK(client.GetFlightInfo({}, CommandDescriptor("CommandStatementQuery")));
  EXPECT_EQ(0U, fake->poll_count);
  EXPECT_EQ(1U, fake->get_count);
}

}  // namespace
}  // namespace arrow::flight::sql::odbc
