# BDX-645 ODBC: production go/no-go and follow-ups

Driver: Arrow C++ Flight SQL ODBC (`apache/arrow`, `cpp/src/arrow/flight/sql/odbc`).
POC branch: `bdx-645-pollinfo-poc`, rebased on 2026-11-25 onto upstream
`ca39779fae`. Review PR: `xborder/arrow#3`.

## Recommendation

**GO for production work, with two conditions.** Progressive PollInfo for ODBC is
feasible. The public ODBC API stays the same, polling is on by default, and
`UsePollInfo=false` restores today's behavior exactly. The production patch should:

1. Ship polling and the `SQLCancel` implementation as **separate upstream PRs**.
   `SQLCancel` is useful without PollInfo: upstream issue
   [apache/arrow#55790](https://github.com/apache/arrow/issues/55790), opened
   2026-11-25, reports that its absence causes duplicate query submissions.
2. Treat parameterized T3 as a **documented limitation, not a blocker** (see below).

Verdict: **feasible with documented limitations**. This is unchanged from the POC.

## T3 decision

Public parameterized T3 fails because the Arrow ODBC driver has never implemented
parameter binding. The driver exports none of `SQLBindParameter`,
`SQLNumParams`, `SQLDescribeParam`, `SQLParamData` or `SQLPutData` (checked
against `odbc.def` on upstream `ca39779fae`). The driver manager therefore
rejects `SQLBindParameter` with `IM001` before any RPC is sent. This has nothing
to do with PollInfo.

- **No upstream issue or PR exists.** Searches of apache/arrow for
  `SQLBindParameter`, "ODBC parameter" and "ODBC bind" find only the
  `SQLBindCol` work (#47716 / PR #48042). The umbrella issue #30622 is closed
  and has no parameter-binding sub-issue.
- **Size of the missing feature: large, about 2–4k lines across several PRs.**
  The driver only converts Arrow values to C buffers today. Parameter binding
  needs the reverse conversion (C or SQL type to Arrow builder) for every
  supported C type. It also needs APD/IPD record handling (the descriptor
  infrastructure exists, but no parameter records do), parameter arrays
  (`SQL_ATTR_PARAMSET_SIZE`), an SPI hook that calls
  `PreparedStatement::SetParameters`, and the new entry points in `odbc.def` and
  the Windows installer. Data-at-execution (`SQLParamData`/`SQLPutData`) can come
  later. For comparison, PR #48042 (`SQLBindCol`) was +554 lines because the
  conversion it needed already existed.
- **The PollInfo contract is proven one layer down.** The new shared-fixture test
  `PollingFlightSqlClientSharedFixtureTest.PreparedParametersBindOnceBeforePolling`
  sends a bound `PreparedStatement` through the same `PollingFlightSqlClient` the
  ODBC connection owns. Result: **PASS**. The server records
  `parameter_binding=1` and `original_descriptors=1`, in the order
  `DoPutPreparedStatementQuery, PollFlightInfo, DoGet, PollFlightInfo, DoGet, PollFlightInfo`,
  and the rows are `[41, 42]`. The bind happens exactly once, before the first
  poll, and continuations never resend the bind or the original command.
- **Decision:** record T3 as an accepted limitation of the current driver, outside
  the PollInfo scope. Once upstream parameter binding exists, run the existing
  `PollInfoConformanceTest.PreparedBindsOnce` unchanged. It needs no
  PollInfo-specific work, because `PreparedStatement::Execute` already binds
  before calling the polling `GetFlightInfo`.

## Validation on the rebased branch (2026-11-25)

The commands, counters and build-environment notes are in
`reports/raw/rebase-2026-11-25.log`. The shared server was built from
`dremio-employees/helder.gregorio#7` at `72e413a` and run on
`127.0.0.1:31347` (Flight) and `127.0.0.1:31348` (control).

| Suite | Result |
| --- | --- |
| Build of `arrow-odbc-spi-impl-test`, `arrow-flight-test` and `arrow-flight-sql-odbc-test` | PASS |
| Focused PollInfo tests (`arrow-odbc-spi-impl-test`) | 17/17 pass; the gated T3 surrogate is skipped without a server |
| Transport `TestCancel.PollFlightInfoWhileBlocked` | 1/1 pass |
| Full ODBC SPI (`arrow-odbc-spi-impl-test`) | 103 pass, 1 skipped (the gated surrogate) |
| Full ODBC (`arrow-flight-sql-odbc-test`) | 714 tests: 385 pass, 326 skipped, 15 disabled, 3 fail. The 3 failures are the pre-existing driver-manager-only `ConnectionInfoTest` cases. |
| Shared T1, T2, T4–T9 (11 cases) | 11/11 pass |
| Shared T3 through public ODBC | BLOCKED: `SQLBindParameter` returns `IM001` and no RPC reaches the server |
| T3 surrogate (C++ layer, shared server) | PASS: `parameter_binding=1`, `original_descriptors=1` |

Key server traces:

- T2: `PollFlightInfo, DoGet, PollFlightInfo, DoGet, PollFlightInfo, DoGet`
  (progressive consumption).
- T5: the prepared family does `Poll`, then `GetFlightInfo` twice (the cache is
  hit on the second call); the metadata family still polls three times.
- T6: `GetFlightInfo, DoGet` only.
- T7: one `PollFlightInfo` and no fallback. For a late error, the failure
  surfaces after one `DoGet`.
- T8: `active_call_terminations=1` at about 1 s.
- T9: `CancelFlightInfo` is observed for both the blocked fetch and the close of
  an incomplete result.

## `GetFlightInfo` coverage

The `reports/call-site-inventory.md` inventory still holds on the rebased
code: `sql/client.cc` and `sql/client.h` did not change upstream. All 13
`FlightSqlClient` command paths dispatch through the overridden virtual
`GetFlightInfo`. Paths that are covered but not reachable from ODBC today:

- `GetPrimaryKeys`, `GetExportedKeys`, `GetImportedKeys`, `GetCrossReference`:
  `SQLPrimaryKeysW`/`SQLForeignKeysW` are exported but still throw
  "not implemented" upstream.
- `ExecuteSubstrait`: ODBC has no entry point for it.

None of these is a gap in the polling layer. They inherit polling as soon as
an ODBC entry point calls them. Paths that only use `DoPut` (updates, ingest)
are out of scope.

## Follow-ups, smallest first

### Upstream sequencing

1. **`SQLCancel` PR** (independent of PollInfo): operation `StopSource`, a gRPC
   `StopToken` to `TryCancel` bridge, and cancellation of an existing result set.
   Reference #55790.
2. **Internal polling helper in the C++ Flight SQL library**:
   `poll_info_internal.{h,cc}`, which provides initial-only fallback, cumulative
   endpoint-prefix validation and a single shrinking deadline. Agree on its export
   boundary with maintainers, since the JDBC/ADBC equivalents follow the same
   contract.
3. **ODBC integration**: `PollingFlightSqlClient`, progressive
   `FlightStreamChunkBuffer`, and the `UsePollInfo` connection property, plus
   documentation in `connection-options.md`.

### Hardening before production

4. Replace the POC's 10 ms polling watcher on the gRPC `StopToken` with
   cancellation integrated into the general unary-call lifecycle, or get it
   formally approved.
5. Make execute, cleanup and close explicitly no-throw. Stress `SQLCancel` racing
   against execute, close and handle destruction on iODBC, unixODBC and Windows.
6. Two first probes for the same unsupported command family can run at the same
   time. Define and test the expected behavior: two `UNIMPLEMENTED` probes are
   acceptable, but each must fall back only once.
7. Check how the unsupported-family cache behaves in pooled and reconnected
   connections. The cache is per physical connection, which matches the contract.
8. Add `SQLCancelHandle` where the target headers and driver manager expose it,
   routed to the same `StopSource`.
9. Keep the focused matrix in upstream CI: initial-only fallback, cumulative
   prefix, shrinking deadline, timeout and cancel cleanup, and late error at the
   next `SQLFetch`.

### Documented limitations

- Parameterized prepared statements are unavailable until upstream parameter
  binding lands (T3 above).
- `SQLCancelHandle` is not implemented. `SQLCancel` is.
- Three `ConnectionInfoTest/0.TestSQLGetInfoDriver{Hdbc,Henv,Hlib}` failures are
  pre-existing and come from the iODBC driver manager. They are unrelated to this
  patch.

## Rebase notes

`origin/bdx-645-pollinfo-poc` (`c057d0be58`, based on `79e074ace3`) was 306
commits behind. There was one conflict, in `flight_sql_stream_chunk_buffer.cc`:
upstream GH-54649 added a `Location::ReuseConnection()` check to the
"fetch on the current connection" branch, while the POC had turned the
`flight_sql_client` and `call_options` locals into members. The resolution keeps
both: upstream's condition, using the POC's members. The other four commits
applied with only context changes.
