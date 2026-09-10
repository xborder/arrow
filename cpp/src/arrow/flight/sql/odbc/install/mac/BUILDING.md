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

## EC2 Mac validation (native Intel and Apple Silicon)

Use a dedicated EC2 Mac host for each architecture; do not use Rosetta as a
replacement for Intel validation. The AMI and host type must match:

| Build | AMI family | EC2 host type |
|---|---|---|
| x86_64 | macOS Sequoia 15.x (`x86_64_mac`) | `mac1.metal` |
| arm64 | macOS Tahoe 26.x (`arm64_mac`) | `mac2.metal`, `mac2-m2.metal`, `mac2-m2pro.metal`, or a supported `mac-m4*` type |

AMI IDs are region-specific. Query the current public Amazon images instead of
copying an ID from another region:

```sh
aws ec2 describe-images --owners amazon \
  --filters Name=state,Values=available Name=name,Values='amzn-ec2-macos-15*-*' \
  --query 'sort_by(Images,&CreationDate)[-1].{Id:ImageId,Name:Name,Arch:Architecture}'
aws ec2 describe-images --owners amazon \
  --filters Name=state,Values=available Name=name,Values='amzn-ec2-macos-26*-arm64' \
  --query 'sort_by(Images,&CreationDate)[-1].{Id:ImageId,Name:Name,Arch:Architecture}'
```

Allocate the Apple Silicon host and wait for `State=available` before launching
the instance. Capacity is availability-zone specific; query offerings and try
another allowed `mac2*`/`mac-m4*` type or AZ if the request is refused:

```sh
aws ec2 describe-instance-type-offerings --location-type availability-zone \
  --filters Name=instance-type,Values=mac2.metal,mac2-m2.metal,mac2-m2pro.metal,mac-m4.metal,mac-m4pro.metal,mac-m4max.metal \
  --query 'InstanceTypeOfferings[].{Type:InstanceType,AZ:Location}'
aws ec2 allocate-hosts --instance-type mac2-m2pro.metal \
  --availability-zone us-east-1c --quantity 1 --auto-placement off \
  --tag-specifications 'ResourceType=dedicated-host,Tags=[{Key=Purpose,Value=arrow-odbc-validation}]'
aws ec2 wait host-available --host-ids <arm-host-id>
```

Launch one instance per host, using a temporary key and an SSH-only security
group. Confirm the architecture and OS from inside each guest before building:

```sh
aws ec2 run-instances --image-id <sequoia-x86-ami> --instance-type mac1.metal \
  --placement HostId=<mac1-host-id> --subnet-id <subnet-in-that-az> \
  --security-group-ids <temporary-ssh-group> --key-name <temporary-key> \
  --associate-public-ip-address --tag-specifications \
  'ResourceType=instance,Tags=[{Key=Purpose,Value=arrow-odbc-validation}]'

aws ec2 run-instances --image-id <tahoe-arm64-ami> --instance-type mac2-m2pro.metal \
  --placement HostId=<arm-host-id> --subnet-id <subnet-in-that-az> \
  --security-group-ids <temporary-ssh-group> --key-name <temporary-key> \
  --associate-public-ip-address --tag-specifications \
  'ResourceType=instance,Tags=[{Key=Purpose,Value=arrow-odbc-validation}]'

ssh -i <temporary-key>.pem ec2-user@<address> 'uname -m; sw_vers; df -h /'
```

Run the architecture-specific configure/build commands in this document on
the corresponding guest. Generate and verify the DMG with `hdiutil create`,
mount it read-only, confirm it contains the expected PKG, and detach it. Then
install the PKG on the clean guest and run the smoke test below. Enter the PAT
only at the silent prompt; never put it in an AMI, command line, log, or file.

When validation ends, terminate both instances, delete the temporary key pair
and security group, and release every dedicated host. EC2 Mac hosts have a
24-hour minimum allocation period; if release is rejected before that period,
record the host ID and release-eligible time and release it afterward.

## Check out the exact source

```sh
git fetch origin codex/arrow-25.0.1-macos-driver-artifacts
git switch --detach origin/codex/arrow-25.0.1-macos-driver-artifacts
git submodule update --init --recursive
test "$(git merge-base HEAD beccec0d0c451b7aa3e4530416ac431b3c035c69)" = \
  beccec0d0c451b7aa3e4530416ac431b3c035c69
```

The branch contains the OpenSSL header-selection fix used by the validated
artifacts. Checking out the release commit directly omits that fix.

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

The ARM64 artifact carries OpenSSL statically. The validated x86_64 diagnostic
artifact dynamically references Intel Homebrew OpenSSL 3 at
`/usr/local/opt/openssl@3`; install that runtime before loading it.

The DMG files are thin distribution wrappers around the corresponding PKG;
double-click the PKG inside the mounted image to install the driver. CPack is
configured by this repository to generate `productbuild` PKGs, not DMGs, so
create the optional DMG after `cpack` with Apple's `hdiutil`:

```sh
BUILD_DIR=build/macos-arm64-release
PKG="$BUILD_DIR/ArrowFlightSQLODBC-25.0.1.pkg"
DMG_STAGE=$(mktemp -d /tmp/arrow-odbc-dmg.XXXXXX)
trap 'rm -rf "$DMG_STAGE"' EXIT
cp "$PKG" "$DMG_STAGE/"
hdiutil create \
  -volname "Arrow Flight SQL ODBC 25.0.1 arm64" \
  -srcfolder "$DMG_STAGE" \
  -format UDZO \
  -ov \
  "$BUILD_DIR/Apache-Arrow-Flight-SQL-ODBC-25.0.1-macos-arm64.dmg"
```

For Intel, use the x86_64 build directory and change the volume name and output
file suffix to `x86_64`. Verify an image before distribution:

```sh
hdiutil attach -readonly -nobrowse "$BUILD_DIR/Apache-Arrow-Flight-SQL-ODBC-25.0.1-macos-arm64.dmg"
hdiutil detach "/Volumes/Arrow Flight SQL ODBC 25.0.1 arm64"
```

The checked-in DMGs are generated this way. They are unsigned and are not a
replacement for signing and notarizing the PKG for production distribution.

Verify all four files from the repository root with:

```sh
shasum -a 256 -c ARTIFACTS.sha256
```

## Sign and notarize for distribution

Use a Developer ID Application identity for Mach-O code and the DMG, and a
separate Developer ID Installer identity for the PKG. Confirm both identities
in the login keychain before starting. Do not print or commit certificate
private keys, App Store Connect passwords, or notarization tokens.

```sh
security find-identity -v -p codesigning
security find-identity -v | grep 'Developer ID Installer'
```

Set these to the exact identity names shown by `security`:

```sh
APP_IDENTITY='Developer ID Application: Your Organization (TEAMID)'
INSTALLER_IDENTITY='Developer ID Installer: Your Organization (TEAMID)'
BUILD_DIR=build/macos-arm64-release
DRIVER="$BUILD_DIR/release/libarrow_flight_sql_odbc.2500.1.0.dylib"
UNSIGNED_PKG="$BUILD_DIR/ArrowFlightSQLODBC-25.0.1.pkg"
SIGNED_PKG="$BUILD_DIR/ArrowFlightSQLODBC-25.0.1-signed.pkg"
DMG="$BUILD_DIR/Apache-Arrow-Flight-SQL-ODBC-25.0.1-macos-arm64.dmg"
```

Sign the driver before CPack copies it into the installer. Sign any nested
Mach-O dependencies from the inside out; this build's ARM64 driver has its
non-system dependencies linked statically.

```sh
codesign --force --timestamp --options runtime \
  --sign "$APP_IDENTITY" "$DRIVER"
codesign --verify --strict --verbose=4 "$DRIVER"

cmake --install "$BUILD_DIR"
cpack --config "$BUILD_DIR/CPackConfig.cmake" -B "$BUILD_DIR"
```

Sign the generated flat installer with the Installer identity:

```sh
productsign --sign "$INSTALLER_IDENTITY" "$UNSIGNED_PKG" "$SIGNED_PKG"
pkgutil --check-signature "$SIGNED_PKG"
```

Create the DMG from the signed PKG, then sign the completed disk image with the
Application identity. A DMG is a container, not a second installer format; the
user opens it and runs the signed PKG inside.

```sh
DMG_STAGE=$(mktemp -d /tmp/arrow-odbc-signed-dmg.XXXXXX)
trap 'rm -rf "$DMG_STAGE"' EXIT
cp "$SIGNED_PKG" "$DMG_STAGE/"
hdiutil create \
  -volname "Arrow Flight SQL ODBC 25.0.1 arm64" \
  -srcfolder "$DMG_STAGE" \
  -format UDZO \
  -ov \
  "$DMG"
codesign --force --timestamp \
  --identifier org.apache.arrow.flight-sql-odbc.dmg \
  --sign "$APP_IDENTITY" "$DMG"
codesign --verify --strict --verbose=4 "$DMG"
```

For the Intel artifact, use the x86_64 build directory, `arch -x86_64` for all
build and signing tools where required, and an x86_64-specific DMG name. The
validated x86_64 diagnostic artifact dynamically references Intel Homebrew
OpenSSL 3 at `/usr/local/opt/openssl@3`; do not distribute it to machines that
do not provide that dependency. Rebuild OpenSSL statically or bundle and sign
the required libraries with `@rpath` before signing the Intel driver.

For direct distribution, store notarization credentials in the keychain rather
than in a script or environment log, then notarize the outermost artifact:

```sh
xcrun notarytool store-credentials arrow-notary \
  --apple-id 'your-apple-id@example.com' \
  --team-id 'TEAMID' \
  --password 'app-specific-password'
xcrun notarytool submit "$DMG" --keychain-profile arrow-notary --wait
xcrun stapler staple "$DMG"
xcrun stapler validate "$DMG"
spctl -a -t open --context context:primary-signature -v "$DMG"
```

Only the outermost container needs notarization when the signed PKG is shipped
inside the DMG. If the PKG is also distributed as a standalone download, submit
and staple that signed PKG separately. Recreate and re-sign the DMG whenever
its contents change. Apple requires Developer ID signatures and a secure
timestamp for notarized distribution; production PKGs and DMGs should also be
tested on a clean Mac with Gatekeeper enabled.

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

Do not set `disableCertificateVerification=true` when using a real PAT. Both
validated architectures completed the authenticated query with certificate
verification enabled.

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
