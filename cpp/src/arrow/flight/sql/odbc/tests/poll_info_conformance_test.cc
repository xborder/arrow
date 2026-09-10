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

#include <gtest/gtest.h>

#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace arrow::flight::sql::odbc {
namespace {

bool IsSuccess(SQLRETURN value) {
  return value == SQL_SUCCESS || value == SQL_SUCCESS_WITH_INFO;
}

std::vector<SQLWCHAR> Wide(const std::string& value) {
  std::vector<SQLWCHAR> result(value.begin(), value.end());
  result.push_back(0);
  return result;
}

std::string Narrow(const SQLWCHAR* value, size_t length) {
  std::string result;
  result.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    result.push_back(static_cast<char>(value[i]));
  }
  return result;
}

class PollInfoConformanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* value = std::getenv("BDX645_CONFORMANCE_PORT");
    if (value == nullptr) {
      GTEST_SKIP() << "Set BDX645_CONFORMANCE_PORT to run shared-fixture tests";
    }
    port_ = std::stoi(value);
  }

  void TearDown() override {
    if (statement_ != SQL_NULL_HSTMT) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, statement_));
    }
    if (connection_ != SQL_NULL_HDBC) {
      EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection_));
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection_));
    }
    if (environment_ != SQL_NULL_HENV) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment_));
    }
  }

  void Connect(bool use_poll_info = true) {
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetEnvAttr(
                  environment_, SQL_ATTR_ODBC_VERSION,
                  reinterpret_cast<SQLPOINTER>(static_cast<intptr_t>(SQL_OV_ODBC3)),
                  0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_));
    const std::string connection_string =
        "DRIVER={Apache Arrow Flight SQL ODBC Driver};HOST=127.0.0.1;PORT=" +
        std::to_string(port_) +
        ";useEncryption=false;UseWideChar=false;UsePollInfo=" +
        (use_poll_info ? "true;" : "false;");
    auto input = Wide(connection_string);
    SQLWCHAR output[2048] = {};
    SQLSMALLINT output_length = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLDriverConnectW(connection_, nullptr, input.data(), SQL_NTS, output,
                                2048, &output_length, SQL_DRIVER_NOPROMPT));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_));
  }

  SQLRETURN ExecuteDirect(const std::string& query) {
    auto sql = Wide(query);
    return SQLExecDirectW(statement_, sql.data(), SQL_NTS);
  }

  std::vector<int64_t> FetchIntegers(SQLUSMALLINT column) {
    std::vector<int64_t> values;
    while (true) {
      SQLRETURN status = SQLFetch(statement_);
      if (status == SQL_NO_DATA) break;
      EXPECT_TRUE(IsSuccess(status));
      if (!IsSuccess(status)) break;
      SQLBIGINT value = 0;
      SQLLEN indicator = 0;
      EXPECT_TRUE(IsSuccess(SQLGetData(statement_, column, SQL_C_SBIGINT, &value,
                                       sizeof(value), &indicator)));
      values.push_back(static_cast<int64_t>(value));
    }
    return values;
  }

  int FetchRowCount() {
    int rows = 0;
    while (true) {
      SQLRETURN status = SQLFetch(statement_);
      if (status == SQL_NO_DATA) break;
      EXPECT_TRUE(IsSuccess(status));
      if (!IsSuccess(status)) return -1;
      ++rows;
    }
    return rows;
  }

  int port_ = 0;
  SQLHENV environment_ = SQL_NULL_HENV;
  SQLHDBC connection_ = SQL_NULL_HDBC;
  SQLHSTMT statement_ = SQL_NULL_HSTMT;
};

TEST_F(PollInfoConformanceTest, Immediate) {
  Connect();
  ASSERT_TRUE(IsSuccess(ExecuteDirect("immediate")));
  EXPECT_EQ((std::vector<int64_t>{1, 2}), FetchIntegers(2));
}

TEST_F(PollInfoConformanceTest, MultiStepFinalCumulative) {
  Connect();
  ASSERT_TRUE(IsSuccess(ExecuteDirect("multi-step")));
  EXPECT_EQ((std::vector<int64_t>{1, 2, 3}), FetchIntegers(2));
}

TEST_F(PollInfoConformanceTest, PreparedBindsOnce) {
  Connect();
  auto sql = Wide("prepared-multi-step");
  ASSERT_TRUE(IsSuccess(SQLPrepareW(statement_, sql.data(), SQL_NTS)));
  SQLBIGINT parameter = 41;
  SQLLEN indicator = 0;
  const SQLRETURN bind_status =
      SQLBindParameter(statement_, 1, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT,
                       0, 0, &parameter, 0, &indicator);
  SQLWCHAR sql_state[6] = {};
  SQLWCHAR message[512] = {};
  SQLINTEGER native_error = 0;
  SQLSMALLINT message_length = 0;
  const SQLRETURN diagnostic_status =
      SQLGetDiagRecW(SQL_HANDLE_STMT, statement_, 1, sql_state, &native_error,
                     message, 512, &message_length);
  ASSERT_TRUE(IsSuccess(bind_status))
      << "SQLBindParameter returned " << bind_status << "; diagnostic_status="
      << diagnostic_status << "; SQLSTATE=" << Narrow(sql_state, 5)
      << "; native_error=" << native_error << "; message="
      << Narrow(message, static_cast<size_t>(std::max<SQLSMALLINT>(message_length, 0)));
  ASSERT_TRUE(IsSuccess(SQLExecute(statement_)));
  EXPECT_EQ((std::vector<int64_t>{41, 42}), FetchIntegers(1));
}

TEST_F(PollInfoConformanceTest, MetadataCatalogs) {
  Connect();
  auto catalogs = Wide("%");
  auto empty = Wide("");
  ASSERT_TRUE(IsSuccess(SQLTablesW(statement_, catalogs.data(), SQL_NTS,
                                   empty.data(), SQL_NTS, empty.data(), SQL_NTS,
                                   nullptr, 0)));
  EXPECT_EQ(1, FetchRowCount());
}

TEST_F(PollInfoConformanceTest, UnsupportedCacheAndDifferentFamily) {
  Connect();
  for (int attempt = 0; attempt < 2; ++attempt) {
    ASSERT_TRUE(IsSuccess(ExecuteDirect("unimplemented")));
    EXPECT_GT(FetchIntegers(2).size(), 0U);
    ASSERT_TRUE(IsSuccess(SQLCloseCursor(statement_)));
  }
  auto catalogs = Wide("%");
  auto empty = Wide("");
  ASSERT_TRUE(IsSuccess(SQLTablesW(statement_, catalogs.data(), SQL_NTS,
                                   empty.data(), SQL_NTS, empty.data(), SQL_NTS,
                                   nullptr, 0)));
  EXPECT_EQ(1, FetchRowCount());
}

TEST_F(PollInfoConformanceTest, UnavailableDoesNotFallback) {
  Connect();
  EXPECT_EQ(SQL_ERROR, ExecuteDirect("unavailable"));
}

TEST_F(PollInfoConformanceTest, ConnectionOptOut) {
  Connect(false);
  ASSERT_TRUE(IsSuccess(ExecuteDirect("immediate")));
  EXPECT_EQ((std::vector<int64_t>{1, 2}), FetchIntegers(2));
}

TEST_F(PollInfoConformanceTest, QueryTimeoutInterruptsBlockedPoll) {
  Connect();
  ASSERT_TRUE(IsSuccess(SQLSetStmtAttr(
      statement_, SQL_ATTR_QUERY_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(static_cast<intptr_t>(1)), 0)));
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(SQL_ERROR, ExecuteDirect("blocked-poll"));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(elapsed, std::chrono::milliseconds(500));
  EXPECT_LT(elapsed, std::chrono::seconds(3));
}

TEST_F(PollInfoConformanceTest, SQLCancelInterruptsBeforeResultSet) {
  Connect();
  SQLRETURN execute_status = SQL_SUCCESS;
  const auto start = std::chrono::steady_clock::now();
  std::thread execute(
      [&] { execute_status = ExecuteDirect("cancel-observable"); });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  EXPECT_TRUE(IsSuccess(SQLCancel(statement_)));
  execute.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(SQL_ERROR, execute_status);
  EXPECT_LT(elapsed, std::chrono::seconds(3));
  SQLSMALLINT columns = -1;
  const SQLRETURN columns_status = SQLNumResultCols(statement_, &columns);
  EXPECT_TRUE(!IsSuccess(columns_status) || columns == 0);
}

}  // namespace
}  // namespace arrow::flight::sql::odbc
