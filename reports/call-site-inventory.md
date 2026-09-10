# BDX-645 Arrow C++ Flight SQL / ODBC GetFlightInfo inventory

Upstream revision: `79e074ace3a7c4ca26f211dfd84a1984ad53c362`.

## C++ Flight SQL

All command methods below serialize their protobuf command in
`cpp/src/arrow/flight/sql/client.cc`, call the private
`GetFlightInfoForCommand`, and ultimately dispatch through virtual
`FlightSqlClient::GetFlightInfo`.  The POC overrides that one virtual method in
`PollingFlightSqlClient`, so each path is covered without changing the public
Flight SQL client API.

| Method/path | Command family (protobuf Any type URL suffix) | Applicability/disposition |
| --- | --- | --- |
| `FlightSqlClient::Execute` | `CommandStatementQuery` | SPI direct query; polls through the override. |
| `FlightSqlClient::ExecuteSubstrait` | `CommandStatementSubstraitPlan` | C++ Flight SQL path; polls through the override, not directly exposed by this ODBC driver. |
| `FlightSqlClient::GetCatalogs` | `CommandGetCatalogs` | ODBC `SQLTables` all-catalogs branch; polls. |
| `FlightSqlClient::GetDbSchemas` | `CommandGetDbSchemas` | ODBC `SQLTables` all-schemas branch; polls. |
| `FlightSqlClient::GetTables` | `CommandGetTables` | ODBC generic `SQLTables` and `SQLColumns`; polls. |
| `FlightSqlClient::GetPrimaryKeys` | `CommandGetPrimaryKeys` | C++ Flight SQL path; polls if invoked through the connection-owned client; no current ODBC entry path. |
| `FlightSqlClient::GetExportedKeys` | `CommandGetExportedKeys` | C++ Flight SQL path; polls; no current ODBC entry path. |
| `FlightSqlClient::GetImportedKeys` | `CommandGetImportedKeys` | C++ Flight SQL path; polls; no current ODBC entry path. |
| `FlightSqlClient::GetCrossReference` | `CommandGetCrossReference` | C++ Flight SQL path; polls; no current ODBC entry path. |
| `FlightSqlClient::GetTableTypes` | `CommandGetTableTypes` | ODBC `SQLTables` all-types branch; polls. |
| `FlightSqlClient::GetXdbcTypeInfo` (both overloads) | `CommandGetXdbcTypeInfo` | ODBC `SQLGetTypeInfo`; polls. |
| `FlightSqlClient::GetSqlInfo` | `CommandGetSqlInfo` | ODBC `GetInfoCache`; polls. |
| `PreparedStatement::Execute` | `CommandPreparedStatementQuery` | Binds its optional RecordBatch exactly once, then calls the same virtual `GetFlightInfo`; polls. |

`ExecuteUpdate`, prepared updates, ingest, and other DoPut-only paths do not
obtain FlightInfo and are outside the frozen scope.  `GetSchema` methods call
`GetSchema`, not `GetFlightInfo`, and are also outside this inventory.

## ODBC entry-to-family mapping

| ODBC/SPI path | Existing path preserved | Cache family |
| --- | --- | --- |
| Public `SQLExecDirectW` | Baseline `Prepare` + `ExecutePrepared` -> `PreparedStatement::Execute` | Full Any type URL for `CommandPreparedStatementQuery` (`prepared` in the shared fixture). |
| Public `SQLPrepareW` + `SQLExecute` | `PreparedStatement::Execute` | `CommandPreparedStatementQuery` (`prepared`). |
| Internal `FlightSqlStatement::Execute` | `FlightSqlClient::Execute` | `CommandStatementQuery` (`direct`). |
| `SQLTablesW` all catalogs | `GetTablesForSQLAllCatalogs` -> `GetCatalogs` | `CommandGetCatalogs` (`metadata`). |
| `SQLTablesW` all schemas | `GetTablesForSQLAllDbSchemas` -> `GetDbSchemas` | `CommandGetDbSchemas` (`metadata`). |
| `SQLTablesW` all table types | `GetTablesForSQLAllTableTypes` -> `GetTableTypes` | `CommandGetTableTypes` (`metadata`). |
| Generic `SQLTablesW` | `GetTablesForGenericUse` -> `GetTables` | `CommandGetTables` (`metadata`). |
| `SQLColumnsW` (ODBC 2/3) | `FlightSqlStatement::GetColumns_V2/V3` -> `GetTables` | `CommandGetTables` (`metadata`). |
| `SQLGetTypeInfo` (ODBC 2/3) | `FlightSqlStatement::GetTypeInfo_V2/V3` -> `GetXdbcTypeInfo` | `CommandGetXdbcTypeInfo` (`metadata`). |
| `SQLGetInfo` cache fill | `GetInfoCache::Initialize` -> `GetSqlInfo` | `CommandGetSqlInfo` (`metadata`). |

The unsupported-family set lives on the one `PollingFlightSqlClient` owned by a
physical `FlightSqlConnection`.  It is keyed by the exact Any type URL, not by a
global boolean.  Thus an initial `UNIMPLEMENTED` for prepared execution does not
disable `CommandGetCatalogs` or any other metadata family.  Shared T5 proves
this with prepared `poll=1/get=2` and metadata `poll=3/get=0` on one physical
connection.

## Result path

Only the final cumulative `FlightInfo` returned by the polling helper is handed
to the existing `FlightSqlResultSet`.  The existing endpoint readers,
`FlightStreamChunkBuffer`, accessors, and fetch APIs are unchanged.  Partial
PollInfo endpoints are never passed to DoGet.
