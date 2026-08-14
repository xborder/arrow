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

#include <windows.h>

#include <sql.h>
#include <sqlext.h>

#include <iostream>
#include <iterator>

namespace {

bool Check(SQLRETURN result, SQLSMALLINT handle_type, SQLHANDLE handle,
           const char* operation) {
  if (SQL_SUCCEEDED(result)) {
    return true;
  }

  std::cerr << operation << " failed (" << result << ")\n";
  for (SQLSMALLINT record = 1;; ++record) {
    SQLWCHAR state[6] = {};
    SQLWCHAR message[1024] = {};
    SQLINTEGER native_error = 0;
    SQLSMALLINT message_length = 0;
    const auto diagnostic = SQLGetDiagRecW(
        handle_type, handle, record, state, &native_error, message,
        static_cast<SQLSMALLINT>(std::size(message)), &message_length);
    if (diagnostic == SQL_NO_DATA) {
      break;
    }
    if (!SQL_SUCCEEDED(diagnostic)) {
      std::cerr << "Unable to retrieve ODBC diagnostics\n";
      break;
    }
    std::wcerr << L"  [" << state << L"] (" << native_error << L") "
               << message << L"\n";
  }
  return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) {
    std::wcerr << L"Usage: odbc_smoke_test.exe <ODBC connection string>\n";
    return 2;
  }

  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  SQLHSTMT statement = SQL_NULL_HSTMT;
  SQLBIGINT value = 0;
  SQLLEN indicator = 0;
  int exit_code = 1;

  if (!Check(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment),
             SQL_HANDLE_ENV, environment, "SQLAllocHandle(environment)") ||
      !Check(SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                           reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0),
             SQL_HANDLE_ENV, environment, "SQLSetEnvAttr") ||
      !Check(SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection),
             SQL_HANDLE_ENV, environment, "SQLAllocHandle(connection)")) {
    goto cleanup;
  }

  {
    SQLWCHAR completed_connection_string[1024] = {};
    SQLSMALLINT completed_length = 0;
    if (!Check(SQLDriverConnectW(
                   connection, nullptr, reinterpret_cast<SQLWCHAR*>(argv[1]),
                   SQL_NTS, completed_connection_string,
                   static_cast<SQLSMALLINT>(std::size(completed_connection_string)),
                   &completed_length, SQL_DRIVER_NOPROMPT),
               SQL_HANDLE_DBC, connection, "SQLDriverConnectW")) {
      goto cleanup;
    }
  }

  if (!Check(SQLAllocHandle(SQL_HANDLE_STMT, connection, &statement),
             SQL_HANDLE_DBC, connection, "SQLAllocHandle(statement)") ||
      !Check(SQLExecDirectW(statement,
                            const_cast<SQLWCHAR*>(reinterpret_cast<const SQLWCHAR*>(
                                L"SELECT 1 AS odbc_smoke_test")),
                            SQL_NTS),
             SQL_HANDLE_STMT, statement, "SQLExecDirectW")) {
    goto cleanup;
  }

  if (!Check(SQLBindCol(statement, 1, SQL_C_SBIGINT, &value, 0, &indicator),
             SQL_HANDLE_STMT, statement, "SQLBindCol") ||
      !Check(SQLFetch(statement), SQL_HANDLE_STMT, statement, "SQLFetch")) {
    goto cleanup;
  }

  std::cout << "Query succeeded; odbc_smoke_test=" << value << "\n";
  exit_code = 0;

cleanup:
  if (statement != SQL_NULL_HSTMT) {
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
  }
  if (connection != SQL_NULL_HDBC) {
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
  }
  if (environment != SQL_NULL_HENV) {
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
  }
  return exit_code;
}
