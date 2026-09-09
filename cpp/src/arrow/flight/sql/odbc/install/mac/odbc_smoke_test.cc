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

#include <sql.h>
#include <sqlext.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

constexpr char kDriver[] = "Apache Arrow Flight SQL ODBC Driver";
constexpr char kHost[] = "data.eu.dremio.cloud";
constexpr char kPort[] = "443";

std::string Redact(std::string message, const std::string& secret) {
  if (secret.empty()) {
    return message;
  }
  for (std::string::size_type position = 0;
       (position = message.find(secret, position)) != std::string::npos;) {
    message.replace(position, secret.size(), "<redacted>");
    position += sizeof("<redacted>") - 1;
  }
  return message;
}

void PrintDiagnostics(SQLSMALLINT handle_type, SQLHANDLE handle,
                      const std::string& secret) {
  if (handle == SQL_NULL_HANDLE) {
    return;
  }
  for (SQLSMALLINT record = 1;; ++record) {
    SQLCHAR state[6] = {};
    SQLCHAR message[1024] = {};
    SQLINTEGER native_error = 0;
    SQLSMALLINT message_length = 0;
    const SQLRETURN result = SQLGetDiagRec(
        handle_type, handle, record, state, &native_error, message,
        static_cast<SQLSMALLINT>(sizeof(message)), &message_length);
    if (result == SQL_NO_DATA) {
      return;
    }
    if (!SQL_SUCCEEDED(result)) {
      std::cerr << "  unable to retrieve ODBC diagnostics\n";
      return;
    }
    std::cerr << "  [" << state << "] (" << native_error << ") "
              << Redact(reinterpret_cast<const char*>(message), secret) << "\n";
  }
}

bool Check(SQLRETURN result, SQLSMALLINT handle_type, SQLHANDLE handle,
           const char* operation, const std::string& secret) {
  if (SQL_SUCCEEDED(result)) {
    return true;
  }
  std::cerr << operation << " failed (" << result << ")\n";
  PrintDiagnostics(handle_type, handle, secret);
  return false;
}

std::basic_string<SQLWCHAR> ToSqlWide(const std::string& value) {
  std::basic_string<SQLWCHAR> wide_value;
  wide_value.reserve(value.size());
  for (const unsigned char character : value) {
    wide_value.push_back(static_cast<SQLWCHAR>(character));
  }
  return wide_value;
}

}  // namespace

int main() {
  const char* token_environment = std::getenv("DREMIO_PAT");
  if (token_environment == nullptr || token_environment[0] == '\0') {
    std::cerr << "DREMIO_PAT must contain a short-lived test credential\n";
    return 2;
  }
  const std::string token(token_environment);
  if (token.find(';') != std::string::npos) {
    std::cerr << "DREMIO_PAT contains a connection-string delimiter\n";
    return 2;
  }
  const std::string connection_string =
      std::string("driver={") + kDriver + "};host=" + kHost + ";port=" + kPort +
      ";token=" + token +
      ";useEncryption=true;disableCertificateVerification=false;"
      "useSystemTrustStore=true;useWideChar=false;";
  const std::basic_string<SQLWCHAR> wide_connection_string =
      ToSqlWide(connection_string);
  const std::basic_string<SQLWCHAR> wide_query =
      ToSqlWide("SELECT 1 AS odbc_smoke_test");

  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  SQLHSTMT statement = SQL_NULL_HSTMT;
  SQLBIGINT value = 0;
  SQLLEN indicator = 0;
  bool connected = false;
  int exit_code = 1;

  if (!Check(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment),
             SQL_HANDLE_ENV, environment, "SQLAllocHandle(environment)", token) ||
      !Check(SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                           reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0),
             SQL_HANDLE_ENV, environment, "SQLSetEnvAttr", token) ||
      !Check(SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection), SQL_HANDLE_ENV,
             environment, "SQLAllocHandle(connection)", token)) {
    goto cleanup;
  }

  if (!Check(SQLDriverConnectW(connection, nullptr,
                               const_cast<SQLWCHAR*>(wide_connection_string.c_str()),
                               SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT),
             SQL_HANDLE_DBC, connection, "SQLDriverConnect", token)) {
    goto cleanup;
  }
  connected = true;
  std::cout << "Connected to " << kHost << ':' << kPort << " with TLS\n";

  if (!Check(SQLAllocHandle(SQL_HANDLE_STMT, connection, &statement), SQL_HANDLE_DBC,
             connection, "SQLAllocHandle(statement)", token) ||
      !Check(SQLExecDirectW(statement, const_cast<SQLWCHAR*>(wide_query.c_str()), SQL_NTS),
             SQL_HANDLE_STMT, statement, "SQLExecDirect", token) ||
      !Check(SQLBindCol(statement, 1, SQL_C_SBIGINT, &value, sizeof(value), &indicator),
             SQL_HANDLE_STMT, statement, "SQLBindCol", token) ||
      !Check(SQLFetch(statement), SQL_HANDLE_STMT, statement, "SQLFetch", token)) {
    goto cleanup;
  }

  if (indicator == SQL_NULL_DATA || value != 1) {
    std::cerr << "Unexpected SELECT 1 result\n";
    goto cleanup;
  }
  std::cout << "Query succeeded; odbc_smoke_test=" << value << "\n";
  exit_code = 0;

cleanup:
  bool cleanup_succeeded = true;
  if (statement != SQL_NULL_HSTMT) {
    cleanup_succeeded &=
        Check(SQLFreeHandle(SQL_HANDLE_STMT, statement), SQL_HANDLE_STMT, statement,
              "SQLFreeHandle(statement)", token);
  }
  if (connection != SQL_NULL_HDBC) {
    if (connected) {
      cleanup_succeeded &= Check(SQLDisconnect(connection), SQL_HANDLE_DBC, connection,
                                 "SQLDisconnect", token);
    }
    cleanup_succeeded &=
        Check(SQLFreeHandle(SQL_HANDLE_DBC, connection), SQL_HANDLE_DBC, connection,
              "SQLFreeHandle(connection)", token);
  }
  if (environment != SQL_NULL_HENV) {
    cleanup_succeeded &=
        Check(SQLFreeHandle(SQL_HANDLE_ENV, environment), SQL_HANDLE_ENV, environment,
              "SQLFreeHandle(environment)", token);
  }
  if (cleanup_succeeded) {
    std::cout << "ODBC cleanup succeeded\n";
  } else {
    exit_code = 1;
  }
  return exit_code;
}
