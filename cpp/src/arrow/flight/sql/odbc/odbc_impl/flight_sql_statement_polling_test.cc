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

#include "arrow/flight/sql/odbc/odbc_impl/flight_sql_statement.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "arrow/flight/sql/odbc/odbc_impl/diagnostics.h"
#include "arrow/flight/sql/odbc/odbc_impl/exceptions.h"

namespace arrow::flight::sql::odbc {
namespace {

class BlockingFlightSqlClient : public FlightSqlClient {
 public:
  BlockingFlightSqlClient() : FlightSqlClient(std::shared_ptr<FlightClient>{}) {}

  arrow::Result<std::unique_ptr<FlightInfo>> GetFlightInfo(
      const FlightCallOptions& options, const FlightDescriptor&) override {
    entered_ = true;
    while (!options.stop_token.IsStopRequested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    exited_ = true;
    return options.stop_token.Poll();
  }

  bool entered() const { return entered_; }
  bool exited() const { return exited_; }

 private:
  std::atomic<bool> entered_ = false;
  std::atomic<bool> exited_ = false;
};

TEST(FlightSqlStatementPollingTest, CancelInterruptsBeforeResultSetCreation) {
  BlockingFlightSqlClient client;
  Diagnostics diagnostics("Apache Arrow", "test", OdbcVersion::V_3);
  MetadataSettings metadata_settings{std::nullopt, 5, false};
  FlightSqlStatement statement(diagnostics, client, FlightClientOptions::Defaults(), {},
                               metadata_settings);

  auto execution = std::async(std::launch::async, [&]() {
    try {
      statement.Execute("SELECT 1");
      return false;
    } catch (const DriverException&) {
      return true;
    }
  });
  for (int i = 0; i < 200 && !client.entered(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(client.entered());
  ASSERT_EQ(nullptr, statement.GetResultSet());

  statement.Cancel();
  ASSERT_EQ(std::future_status::ready,
            execution.wait_for(std::chrono::milliseconds(500)));
  EXPECT_TRUE(execution.get());
  EXPECT_TRUE(client.exited());
  EXPECT_EQ(nullptr, statement.GetResultSet());
}

}  // namespace
}  // namespace arrow::flight::sql::odbc
