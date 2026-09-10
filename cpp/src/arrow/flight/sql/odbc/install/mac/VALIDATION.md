<!--
Licensed to the Apache Software Foundation (ASF) under one
or more contributor license agreements.  See the NOTICE file
distributed with this work for additional information
regarding copyright ownership.  The ASF licenses this file
to you under the Apache License, Version 2.0 (the
"License"); you may not use this file except in compliance
with the License.  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the License is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied.  See the License for the
specific language governing permissions and limitations
under the License.
-->

# macOS validation evidence: Arrow Flight SQL ODBC 25.0.1

Validation date: 2026-09-10 (Europe/Lisbon)

## Scope and result

Source identity was verified before building:

- commit: `beccec0d0c451b7aa3e4530416ac431b3c035c69`
- exact tag: `apache-arrow-25.0.1`
- branch used for the evidence: `codex/arrow-25.0.1-odbc-macos-validation`
- the separate Windows validation work was not modified

The native ARM64 and x86_64 Release drivers and unsigned installer PKGs built
successfully. The PKG payloads, Mach-O architectures, ODBC exports, install
names, code-signature state, and private iODBC registration/load paths were
checked. The Intel build and smoke test ran under Rosetta because the repository
CI matrix explicitly supports both Intel and ARM macOS.

The corrected end-to-end result is **a pass** on native ARM64 and x86_64 under
Rosetta. A user-supplied credential was injected only through each smoke-test
process environment. With certificate verification enabled, both drivers
connected to `data.eu.dremio.cloud:443`, executed
`SELECT 1 AS odbc_smoke_test`, returned `1`, and released all ODBC resources.

## Host and toolchain

- macOS 26.1 (build 25B78)
- Apple Silicon ARM64, native execution (`sysctl.proc_translated=0`)
- AppleClang 17.0.0 (`clang-1700.6.3.2`)
- CMake 4.0.2
- native Ninja 1.13.2
- Homebrew iODBC 3.52.16
- Homebrew OpenSSL 3.6.1
- Intel Homebrew iODBC 3.52.16 and OpenSSL 3.5.2 under Rosetta
- configured deployment target: macOS 14.0

The ARM64 linker warned that static Homebrew OpenSSL 3.6.1 and utf8proc objects
were built for macOS 26 while the output deployment target was 14.0. The x86_64
linker gave the same warning for utf8proc. Both Mach-O load commands say
`minos 14.0`, but these local artifacts must not be represented as
runtime-compatible with macOS 14. They were exercised only on macOS 26.1,
natively for ARM64 and under Rosetta for x86_64.

## Build configuration

The recorded ARM64 CMake cache contained:

```text
ARROW_DEPENDENCY_SOURCE=BUNDLED
ARROW_DEPENDENCY_USE_SHARED=OFF
ARROW_FLIGHT_SQL_ODBC=ON
ARROW_FLIGHT_SQL_ODBC_INSTALLER=ON
CMAKE_BUILD_TYPE=Release
CMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja
CMAKE_OSX_ARCHITECTURES=arm64
CMAKE_OSX_DEPLOYMENT_TARGET=14.0
```

Several host-pollution problems were isolated without changing the user's
global headers:

- the pre-existing `/usr/local/bin/ninja` was Intel-only and caused nested
  external projects to inject x86_64 flags into the ARM build; a native Ninja
  was selected explicitly;
- stale `/usr/local/include/absl` and `/usr/local/include/google` symlinks took
  precedence over bundled Abseil 20250127.0 and Protobuf 31.1;
- stale `/usr/local/include/sqltypes.h` took precedence over the selected
  Homebrew iODBC headers;
- an ignored, build-only first-include directory selected the bundled Abseil
  and Protobuf trees plus the intended iODBC headers;
- gRPC linked the selected OpenSSL 3 libraries but its TLS sources directly
  included stale OpenSSL 1.1 headers from `/usr/local/include/openssl`. The
  resulting header/library mismatch broke peer-certificate extraction and
  hostname verification. `ThirdpartyToolchain.cmake` now places the selected
  `OPENSSL_INCLUDE_DIR` first for the `grpc` and `grpc++` targets;
- the gRPC 1.76.0 archive was retried after a partial transfer and verified as
  SHA-256 `0af37b800953130b47c075b56683ee60bdc3eda3c37fc6004193f5b569758204`.

## ARM64 artifact checks

Build target:

```text
cmake --build build/macos-arm64-release \
  --target arrow_flight_sql_odbc_shared -j 8
```

Results:

- driver: Mach-O 64-bit dynamically linked shared library, `arm64`
- driver size: approximately 45.6 MiB
- package: `ArrowFlightSQLODBC-25.0.1.pkg`, approximately 12.4 MiB
- package signature: none
- driver signature: valid ad-hoc linker signature, no Team ID
- install name: `@rpath/libarrow_flight_sql_odbc.2500.dylib`
- compatibility/current versions: `2500.0.0` / `2500.1.0`
- expected exports observed, including `SQLAllocHandle`, `SQLDriverConnectW`,
  `SQLExecDirectW`, `SQLFetch`, and `SQLFreeHandle`
- no non-system dynamic Arrow/gRPC/OpenSSL dependencies appeared in `otool -L`

## x86_64 artifact checks

The Intel build completed under Rosetta and produced a 44.4 MiB x86_64 Mach-O
driver and an 11.2 MiB Apple installer package. It has the same install name,
library versions, expected ODBC exports, and configured `minos 14.0` as the
ARM64 artifact. Unlike the ARM64 output's ad-hoc linker signature, the x86_64
driver is unsigned; both installer packages are unsigned. This diagnostic
x86_64 build dynamically references Intel Homebrew OpenSSL 3 under
`/usr/local/opt/openssl@3`, so that runtime is required. The x86_64 smoke
executable compiled cleanly with `-Wall -Wextra -Werror`, loaded the driver
through Intel iODBC under Rosetta, and passed the secure query.

The deliverable checksums are recorded in the repository-root
`ARTIFACTS.sha256` file.

The deliverable PKGs were recreated with Apple's `pkgbuild` and `productbuild`
from the previously inspected component payloads, replacing only the versioned
driver with its corrected architecture-specific binary. Each rebuilt package
was expanded again and its embedded driver compared byte-for-byte with the
corresponding repository-root dylib.

The expanded PKG contains:

```text
/Library/ODBC/arrow-odbc/lib/libarrow_flight_sql_odbc.2500.1.0.dylib
/Library/ODBC/arrow-odbc/lib/libarrow_flight_sql_odbc.2500.dylib -> ...2500.1.0.dylib
/Library/ODBC/arrow-odbc/lib/libarrow_flight_sql_odbc.dylib -> ...2500.dylib
/Library/ODBC/arrow-odbc/doc/connection-options.md
/Library/ODBC/arrow-odbc/doc/LICENSE.txt
/Library/ODBC/arrow-odbc/doc/NOTICE.txt
```

The component identifiers are:

```text
com.Apache Software Foundation.Apache-Arrow-Flight-SQL-ODBC.ArrowFlightSQLODBC
com.Apache Software Foundation.Apache-Arrow-Flight-SQL-ODBC.Docs
```

The package requests root authorization. Passwordless `sudo` was not available,
so the system PKG installation and its postinstall mutation of
`/Library/ODBC/odbcinst.ini` and `/Library/ODBC/odbc.ini` were not run. Payload
expansion verified the exact install tree and scripts. Runtime registration was
tested with a private `ODBCINSTINI` file pointing at the build-tree driver, so
no user or system ODBC configuration was changed.

## Driver-manager and connection evidence

The smoke program compiled cleanly with `-Wall -Wextra -Werror`. Linking it to
only `libiodbc` reproduced a load failure for the driver's unresolved
`_SQLGetPrivateProfileStringW`. Linking both `libiodbc` and `libiodbcinst`, as
the repository's macOS ODBC test CMake does, loaded the registered driver.
Consumers must make the profile library visible when loading this build.

The driver connection-string parser uses a greedy braced-value expression.
Bracing both the driver name and PAT caused the first value to consume through
the PAT and yielded `Missing required properties: host, port`. The sample keeps
the required braced driver name, rejects PATs containing the semicolon
delimiter, and passes the PAT unbraced. It never prints the connection string.

Tests and observations:

1. With `DREMIO_PAT` absent, the sample exited 2 before ODBC allocation and
   printed only that a short-lived credential is required.
2. With private driver registration, certificate verification enabled, and a
   user-supplied credential injected only through the environment, iODBC loaded
   each architecture's corrected driver and connected to
   `data.eu.dremio.cloud:443`. Both completed `SELECT 1`, returned the integer
   `1`, disconnected, and freed their ODBC handles successfully.
3. An independent OpenSSL 3.6.1 handshake to the same endpoint and SNI succeeded
   with TLS 1.2. The certificate was issued by Amazon RSA 2048 M04, had subject
   `CN=*.aws.eu.dremio.cloud`, and had SANs `*.aws.eu.dremio.cloud` and
   `*.eu.dremio.cloud`. It was valid from 2026-05-08 through 2026-11-21, and
   OpenSSL's `-checkhost data.eu.dremio.cloud` explicitly reported a match. This
   narrows the mismatch to the Arrow/gRPC client path rather than an invalid,
   expired, or non-matching endpoint certificate.
4. Symbol inspection of both corrected binaries found
   `SSL_get1_peer_certificate`, the OpenSSL 3 API, and did not find the legacy
   `SSL_get_peer_certificate` path selected by the stale headers.

The exact secure live-test shape is in `BUILDING.md`. It retains
`disableCertificateVerification=false`, reads `DREMIO_PAT` silently, runs
`SELECT 1 AS odbc_smoke_test`, validates the returned integer, unsets the
environment variable, and reports cleanup. It passed with the supplied
credential on both architectures.

## Credential and cleanup record

The supplied credential was read silently into each smoke-test process, was not
written to disk or printed, and was absent from the child environment after each
process exited. The sample contains no credential, username, project ID, or
secret output. Certificate verification remained enabled for both successful
tests.

No system package was installed and no system/user ODBC configuration was
changed. Build-only private registration files remain under ignored `build/`
directories. Homebrew prerequisites installed specifically for this validation
were removed after artifact generation; pre-existing OpenSSL was retained.

## Remaining validation

The remaining distribution checks are:

1. Install each PKG with administrator privileges and verify the postinstall
   registration in `/Library/ODBC`.
2. Repeat runtime loading and the authenticated query natively on Intel macOS;
   Rosetta build/load evidence alone is not a native Intel runtime result.
