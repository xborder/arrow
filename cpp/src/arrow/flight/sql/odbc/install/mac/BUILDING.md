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

# Building and validating Flight SQL ODBC 25.0.1 on macOS

This runbook builds the Apache Arrow Flight SQL ODBC driver from commit
`beccec0d0c451b7aa3e4530416ac431b3c035c69`, the commit tagged
`apache-arrow-25.0.1`. It produces a native Mach-O driver and the repository's
`productbuild` PKG. The commands use Homebrew iODBC and build Arrow's other
dependencies statically, matching the intent of the macOS ODBC CI job.

The checked-in artifacts were built separately for Apple Silicon and Intel.
Do not combine them into a universal binary without repeating the load and
query checks on that combined binary.

## Prerequisites

- Xcode Command Line Tools / AppleClang 17 or newer
- CMake 4
- Ninja
- Homebrew `libiodbc` and `openssl@3`
- About 4 GiB of free disk space per architecture
- A short-lived Dremio PAT with permission to execute `SELECT 1`

On Apple Silicon:

```sh
brew install cmake ninja libiodbc openssl@3
```

For an Intel build on Apple Silicon, Rosetta and a separate Intel Homebrew
installation under `/usr/local` are required. Run the build tools themselves
under Rosetta; do not mix `/opt/homebrew` ARM libraries into the Intel build.

## Check out the exact source

```sh
git fetch --tags upstream
git switch --detach beccec0d0c451b7aa3e4530416ac431b3c035c69
test "$(git rev-parse HEAD)" = beccec0d0c451b7aa3e4530416ac431b3c035c69
test "$(git describe --tags --exact-match HEAD)" = apache-arrow-25.0.1
```

Use a new build directory for each architecture. The examples below use
`build/macos-arm64-release`; replace it with `build/macos-x86_64-release` and
make the prefix substitutions shown in the table for Intel.

| Setting | Apple Silicon | Intel under Rosetta |
|---|---|---|
| tool invocation | native | `arch -x86_64` |
| Homebrew prefix | `/opt/homebrew` | `/usr/local` |
| architecture | `arm64` | `x86_64` |
| Ninja | `/opt/homebrew/bin/ninja` | `/usr/local/bin/ninja` |

## Configure and build

```sh
IODBC_PREFIX=/opt/homebrew/opt/libiodbc
OPENSSL_PREFIX=/opt/homebrew/opt/openssl@3
BUILD_DIR=build/macos-arm64-release

cmake -S cpp -B "$BUILD_DIR" -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_INSTALL_PREFIX=/tmp/arrow-25.0.1-macos-arm64 \
  -DCMAKE_PREFIX_PATH="$IODBC_PREFIX" \
  -DCMAKE_LIBRARY_PATH="$IODBC_PREFIX/lib" \
  -DOPENSSL_ROOT_DIR="$OPENSSL_PREFIX" \
  -DODBC_INCLUDE_DIR="$IODBC_PREFIX/include" \
  -DARROW_BUILD_SHARED=ON \
  -DARROW_BUILD_STATIC=ON \
  -DARROW_BUILD_TESTS=OFF \
  -DARROW_BUILD_UTILITIES=OFF \
  -DARROW_COMPUTE=ON \
  -DARROW_CSV=OFF \
  -DARROW_DATASET=OFF \
  -DARROW_DEPENDENCY_SOURCE=BUNDLED \
  -DARROW_DEPENDENCY_USE_SHARED=OFF \
  -DARROW_FILESYSTEM=OFF \
  -DARROW_FLIGHT=ON \
  -DARROW_FLIGHT_SQL=ON \
  -DARROW_FLIGHT_SQL_ODBC=ON \
  -DARROW_FLIGHT_SQL_ODBC_INSTALLER=ON \
  -DARROW_HDFS=OFF \
  -DARROW_JSON=OFF \
  -DARROW_MIMALLOC=OFF \
  -DARROW_S3=OFF \
  -DARROW_USE_CCACHE=OFF \
  -DARROW_WITH_UTF8PROC=ON \
  -DBUILD_WARNING_LEVEL=PRODUCTION \
  -DZLIB_SOURCE=SYSTEM

cmake --build "$BUILD_DIR" --target arrow_flight_sql_odbc_shared -j 8
cpack --config "$BUILD_DIR/CPackConfig.cmake" -B "$BUILD_DIR"
```

For Intel, use `/usr/local` paths, `x86_64`, an Intel Ninja, and invoke both
CMake commands as `arch -x86_64 /usr/local/bin/cmake ...`. A machine with stale
global headers in `/usr/local/include` may accidentally mix Abseil, Protobuf,
or ODBC headers into the bundled build. Remove those stale packages or put the
selected bundled and iODBC headers in an explicit first include directory; the
validation evidence documents the latter workaround used on the test host.

Verify the output before packaging or installing it:

```sh
file "$BUILD_DIR/release/libarrow_flight_sql_odbc.2500.1.0.dylib"
lipo -archs "$BUILD_DIR/release/libarrow_flight_sql_odbc.2500.1.0.dylib"
otool -L "$BUILD_DIR/release/libarrow_flight_sql_odbc.2500.1.0.dylib"
pkgutil --payload-files "$BUILD_DIR/ArrowFlightSQLODBC-25.0.1.pkg"
shasum -a 256 \
  "$BUILD_DIR/release/libarrow_flight_sql_odbc.2500.1.0.dylib" \
  "$BUILD_DIR/ArrowFlightSQLODBC-25.0.1.pkg"
```

The locally generated PKG is unsigned. Production distribution requires an
Apple Developer Installer signature and notarization.

## Deliverables

The validation branch stores separate artifacts at the repository root:

| Architecture | Installer | Driver |
|---|---|---|
| ARM64 | `Apache-Arrow-Flight-SQL-ODBC-25.0.1-macos-arm64.pkg` | `libarrow_flight_sql_odbc.2500.1.0-macos-arm64.dylib` |
| x86_64 | `Apache-Arrow-Flight-SQL-ODBC-25.0.1-macos-x86_64.pkg` | `libarrow_flight_sql_odbc.2500.1.0-macos-x86_64.dylib` |

Verify all four files from the repository root with:

```sh
shasum -a 256 -c ARTIFACTS.sha256
```

## Install and verify registration

The PKG requires administrator privileges and installs into `/Library/ODBC`.
Back up any existing configuration before installing:

```sh
BACKUP_DIR=$(mktemp -d /tmp/arrow-odbc-backup.XXXXXX)
sudo test ! -f /Library/ODBC/odbcinst.ini || \
  sudo cp /Library/ODBC/odbcinst.ini "$BACKUP_DIR/odbcinst.ini"
sudo test ! -f /Library/ODBC/odbc.ini || \
  sudo cp /Library/ODBC/odbc.ini "$BACKUP_DIR/odbc.ini"

sudo installer -pkg "$BUILD_DIR/ArrowFlightSQLODBC-25.0.1.pkg" -target /

grep -A3 '^\[Apache Arrow Flight SQL ODBC Driver\]$' \
  /Library/ODBC/odbcinst.ini
test -f /Library/ODBC/arrow-odbc/lib/libarrow_flight_sql_odbc.dylib
file /Library/ODBC/arrow-odbc/lib/libarrow_flight_sql_odbc.dylib
```

The registration should contain this non-secret shape:

```ini
[Apache Arrow Flight SQL ODBC Driver]
Description=An ODBC Driver for Apache Arrow Flight SQL
Driver=/Library/ODBC/arrow-odbc/lib/libarrow_flight_sql_odbc.dylib
```

## Build and run the smoke test

The driver has a runtime reference to iODBC's profile API. Link the smoke test
to both `libiodbc` and `libiodbcinst`, as Arrow's macOS ODBC tests do:

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 \
  -I/opt/homebrew/opt/libiodbc/include \
  cpp/src/arrow/flight/sql/odbc/install/mac/odbc_smoke_test.cc \
  -L/opt/homebrew/opt/libiodbc/lib -liodbc -liodbcinst \
  -o build/macos-arm64-release/odbc_smoke_test
```

For Intel, run the compiler under `arch -x86_64`, use `/usr/local/opt/libiodbc`,
and pass `-arch x86_64`.

The program reads a PAT only from `DREMIO_PAT`, never writes the connection
string, redacts the exact PAT from ODBC diagnostics, connects to
`data.eu.dremio.cloud:443` with TLS, certificate verification, and the macOS
system trust store enabled, runs `SELECT 1 AS odbc_smoke_test`, validates the
returned value, and releases all ODBC handles.

Use a silent prompt so the credential is neither committed nor saved in shell
history:

```sh
(
  read -rs 'DREMIO_PAT?Short-lived Dremio PAT: '
  printf '\n'
  export DREMIO_PAT
  build/macos-arm64-release/odbc_smoke_test
)
```

Do not set `disableCertificateVerification=true` when using a real PAT. The
validation host reached the service only with that diagnostic bypass and a
literal invalid test value; the secure attempt failed hostname verification.
That blocker must be resolved before claiming an authenticated query pass.

## Cleanup

The package has no uninstaller. Remove only its exact payload and receipts,
then restore the configuration snapshot:

```sh
sudo rm -rf /Library/ODBC/arrow-odbc
sudo pkgutil --forget \
  'com.Apache Software Foundation.Apache-Arrow-Flight-SQL-ODBC.ArrowFlightSQLODBC'
sudo pkgutil --forget \
  'com.Apache Software Foundation.Apache-Arrow-Flight-SQL-ODBC.Docs'

sudo test ! -f "$BACKUP_DIR/odbcinst.ini" || \
  sudo cp "$BACKUP_DIR/odbcinst.ini" /Library/ODBC/odbcinst.ini
sudo test ! -f "$BACKUP_DIR/odbc.ini" || \
  sudo cp "$BACKUP_DIR/odbc.ini" /Library/ODBC/odbc.ini
```

If either configuration file did not exist before installation, remove only
the driver/DSN stanzas inserted by the package instead of deleting unrelated
ODBC configuration. Delete the architecture-specific build directory when its
logs are no longer needed.
