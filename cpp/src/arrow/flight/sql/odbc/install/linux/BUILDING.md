# Apache Arrow Flight SQL ODBC 25.0.1: Linux build, package, and install

This runbook builds the Apache Arrow Flight SQL ODBC driver from Arrow commit
`beccec0d0c451b7aa3e4530416ac431b3c035c69` and produces x86_64 Linux shared
library, relocatable tar, and standalone RPM deliverables. It never puts a
Dremio credential in a source file, command-line argument, shell history, log,
or artifact.

## Tested target

- Target ABI: Linux x86_64 (`ELF 64-bit LSB shared object, x86-64`).
- Distribution: Ubuntu 24.04 LTS.
- Toolchain: GCC/G++ 13.3.0, CMake 3.28.3, Ninja 1.11.1, glibc 2.39, and
  unixODBC 2.3.12.
- Build mode: Release, shared Arrow Flight SQL ODBC driver with Arrow, Flight,
  Flight SQL, gRPC, and Protobuf linked statically. libcurl, libodbcinst, the C++
  runtime, and the standard system libraries remain dynamic.

The repository's `.github/workflows/cpp_extra.yml` defines its `odbc-linux` CI
job as `amd64` on Ubuntu 24.04. The corresponding `ubuntu-cpp-odbc` service in
`compose.yaml` uses bundled dependencies and disables shared dependency linkage.
That is the repository-defined CI target used here.

The recorded validation used an amd64 Ubuntu container executed through Rosetta
inside an aarch64 Colima Linux VM on Apple Silicon. The compiler, linker,
unixODBC driver manager, driver, smoke-test process, and userspace were all
x86_64. This is an x86_64 ABI validation, but it is not a bare-metal x86_64
performance test.

The standalone RPM was built and tested separately on x86_64 Amazon Linux 2023
VMs (GCC 11.5.0, CMake 3.31.10 in the initial run and 4.4.3 in the final
artifact rebuild, Ninja 1.10.2, glibc 2.34, RPM 4.16.1.3, and unixODBC 2.3.9).
That EL-compatible build is the compatibility baseline for the RPM; the Ubuntu
binary above must not be repackaged for older RPM distributions.

The 25.0.1 source commit predates the standalone RPM helper. Keep this
packaging checkout at the branch containing `build_rpm.sh` and, when producing
the 25.0.1 RPM, use a second clean checkout of the upstream 25.0.1 source for
the CMake build. The helper copies its spec and documentation from the
packaging checkout. A single current-`main` checkout is also supported; use
its reported version when invoking `build_rpm.sh`.

## Prerequisites

On a native Ubuntu 24.04 x86_64 host, install Git, Docker Engine with Compose,
and CA certificates. Allow roughly 12 GiB RAM and 30 GiB free disk space because
the Unix driver build compiles bundled static dependencies.

On Apple Silicon, a disposable Colima VM can provide amd64 container execution:

```bash
brew install colima docker docker-compose
colima start arrow-odbc-linux \
  --arch aarch64 --vm-type vz --vz-rosetta \
  --cpu 10 --memory 12 --disk 80
```

No AWS EC2 resource is required by this procedure.

## Verify the source

```bash
git switch --detach beccec0d0c451b7aa3e4530416ac431b3c035c69
test "$(git rev-parse HEAD)" = beccec0d0c451b7aa3e4530416ac431b3c035c69
git status --short
```

Do not continue from a dirty worktree unless the changes are understood and
intended. The commands below write build products only under
`cpp/build/linux-odbc-validation`.

## Build the x86_64 driver

Build the repository-provided Ubuntu 24.04 toolchain image:

```bash
ARCH=amd64 ARCH_SHORT=amd64 UBUNTU=24.04 \
  docker-compose build ubuntu-cpp-odbc
```

Build and install into the disposable container. The build directory is mounted
back to the host so the driver remains available after the container exits.

```bash
mkdir -p cpp/build/linux-odbc-validation
docker run --rm --platform linux/amd64 \
  -v "$PWD:/arrow" \
  -v "$PWD/cpp/build/linux-odbc-validation:/build" \
  -e ARROW_ACERO=OFF \
  -e ARROW_AZURE=OFF \
  -e ARROW_BUILD_PARALLEL=4 \
  -e ARROW_BUILD_SHARED=ON \
  -e ARROW_BUILD_STATIC=ON \
  -e ARROW_BUILD_TESTS=OFF \
  -e ARROW_BUILD_TYPE=release \
  -e ARROW_BUILD_UTILITIES=OFF \
  -e ARROW_CSV=OFF \
  -e ARROW_DATASET=OFF \
  -e ARROW_DEPENDENCY_SOURCE=BUNDLED \
  -e ARROW_DEPENDENCY_USE_SHARED=OFF \
  -e ARROW_FLIGHT=ON \
  -e ARROW_FLIGHT_SQL=ON \
  -e ARROW_FLIGHT_SQL_ODBC=ON \
  -e ARROW_FLIGHT_SQL_ODBC_INSTALLER=OFF \
  -e ARROW_GANDIVA=OFF \
  -e ARROW_GCS=OFF \
  -e ARROW_HDFS=OFF \
  -e ARROW_HOME=/usr/local \
  -e ARROW_JEMALLOC=OFF \
  -e ARROW_MIMALLOC=OFF \
  -e ARROW_ORC=OFF \
  -e ARROW_PARQUET=OFF \
  -e ARROW_S3=OFF \
  -e ARROW_SUBSTRAIT=OFF \
  -e ARROW_USE_CCACHE=OFF \
  -e CMAKE_BUILD_PARALLEL_LEVEL=4 \
  apache/arrow-dev:amd64-ubuntu-24.04-cpp \
  /arrow/ci/scripts/cpp_build.sh /arrow /build
```

The unstripped build result is:

```text
cpp/build/linux-odbc-validation/cpp/release/libarrow_flight_sql_odbc.so
```

## Package and checksum

The upstream CPack Linux installer branch still emits TODO messages for DEB and
RPM and selects no Linux CPack generator. The relocatable tar is produced by
`package.sh`; this branch additionally provides `build_rpm.sh` and an RPM spec
for a complete standalone RPM installation.

### Standalone RPM matching the Dremio package layout

`build_rpm.sh` creates a standalone x86_64 RPM with the same basic layout as
Dremio's Linux driver package: `/opt/arrow-flight-sql-odbc-driver/lib64` holds
the versioned driver and `/opt/arrow-flight-sql-odbc-driver/conf` holds the
driver and sample DSN configuration. RPM `%post` and `%postun` scripts register
and unregister the driver with unixODBC. The sample DSN is registered on first
install and can then be edited in `/etc/odbc.ini`.

Build on the oldest RPM-based distribution that the package must support. The
Ubuntu 24.04 validation binary in this directory requires newer glibc and
libstdc++ symbols than an older RHEL/CentOS system; putting that binary in an
RPM does not make it compatible with those systems. The following example uses
an x86_64 EL-compatible host or VM (the commands below were validated on
Amazon Linux 2023):

```bash
sudo dnf install -y \
  gcc gcc-c++ cmake ninja-build make git rpm-build file \
  unixODBC unixODBC-devel libcurl-devel openssl-devel ca-certificates \
  pkgconf-pkg-config zlib-devel boost-devel libicu-devel
```

On AlmaLinux 9, enable the CRB repository first if `boost-devel` or
`libicu-devel` is not available from the enabled repositories.

Arrow requires CMake 3.25 or newer. If the distribution's CMake is older
(Amazon Linux 2023 currently ships 3.22), install a newer CMake before running
the build, for example:

```bash
sudo dnf install -y python3-pip
python3 -m pip install --user cmake
export PATH="$HOME/.local/bin:${PATH}"
cmake --version
```

Configure and build the driver from the source checkout. Keep the RPM
installer disabled; RPM registration is supplied by the spec below. On Amazon
Linux 2023, pass the system OpenSSL paths explicitly so CMake 3.31 does not
select an incomplete alternative installation:

```bash
# Run these from the packaging checkout. For the exact 25.0.1 build, set
# source_root to a separate upstream 25.0.1 checkout. For current main, use
# source_root="$PWD" and pass its version to build_rpm.sh below.
packaging_root="$PWD"
source_root="/path/to/apache-arrow-25.0.1"
cd "${source_root}"
```

```bash
cmake -S "${source_root}/cpp" -B "${source_root}/cpp/build/rpm-odbc" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DARROW_BUILD_SHARED=ON \
  -DARROW_BUILD_STATIC=ON \
  -DARROW_BUILD_TESTS=OFF \
  -DARROW_BUILD_EXAMPLES=OFF \
  -DARROW_FLIGHT=ON \
  -DARROW_FLIGHT_SQL=ON \
  -DARROW_FLIGHT_SQL_ODBC=ON \
  -DARROW_FLIGHT_SQL_ODBC_INSTALLER=OFF \
  -DARROW_DEPENDENCY_SOURCE=BUNDLED \
  -DARROW_DEPENDENCY_USE_SHARED=OFF \
  -DBoost_SOURCE=BUNDLED \
  -DARROW_MIMALLOC=OFF \
  -DARROW_JEMALLOC=OFF \
  -DARROW_WITH_OPENTELEMETRY=OFF \
  -DARROW_WITH_THRIFT=OFF \
  -DARROW_WITH_BROTLI=OFF \
  -DARROW_WITH_BZ2=OFF \
  -DARROW_WITH_LZ4=OFF \
  -DARROW_WITH_SNAPPY=OFF \
  -DARROW_WITH_ZSTD=OFF \
  -DOPENSSL_ROOT_DIR=/usr \
  -DOPENSSL_INCLUDE_DIR=/usr/include \
  -DOPENSSL_SSL_LIBRARY=/usr/lib64/libssl.so \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib64/libcrypto.so
cmake --build "${source_root}/cpp/build/rpm-odbc" \
  --target arrow_flight_sql_odbc_shared --parallel "$(nproc)"
driver="$(find "${source_root}/cpp/build/rpm-odbc" -type f \
  -name 'libarrow_flight_sql_odbc.so.*' -print -quit)"
test -n "${driver}"
```

The helper and RPM templates are additions on this branch, while the
reproducible 25.0.1 driver source is the upstream commit shown at the top of
this file. If you build a different checkout, pass the matching semantic
version (for example `26.0.0` for a `26.0.0-SNAPSHOT` checkout) as the third
argument; never label a binary with a version different from the source it was
built from.

Create the RPM and checksum. The optional third argument is the driver version;
it defaults to `25.0.1` for the validated artifact in this directory:

```bash
rpm_dir="${packaging_root}/cpp/src/arrow/flight/sql/odbc/install/linux/artifacts/rpm"
mkdir -p "${rpm_dir}"
"${packaging_root}/cpp/src/arrow/flight/sql/odbc/install/linux/build_rpm.sh" \
  "${driver}" "${rpm_dir}" 25.0.1
rpm_file="$(find "${rpm_dir}" -maxdepth 1 \
  -name 'apache-arrow-flight-sql-odbc-driver-25.0.1-*.rpm' -print -quit)"
test -n "${rpm_file}"
rpm -qip "${rpm_file}"
rpm -qp --requires "${rpm_file}"
(cd "${rpm_dir}" && sha256sum --check RPM-SHA256SUMS)
```

The validated 25.0.1 package is checked in under
`artifacts/rpm/`; verify it before distribution with the same checksum command.
The RPM filename includes the build host's distribution release (the validated
Amazon Linux 2023 build ends in `1.amzn2023.x86_64.rpm`).

Install and validate the package lifecycle as root:

```bash
sudo dnf install -y "${rpm_file}"
odbcinst -q -d -n "Apache Arrow Flight SQL ODBC Driver"
odbcinst -q -s -n "Apache Arrow Flight SQL ODBC DSN"
ldd /opt/arrow-flight-sql-odbc-driver/lib64/libarrow_flight_sql_odbc.so

# Reinstall/upgrade: the driver registration must remain single and valid.
sudo dnf install -y "${rpm_file}"
test "$(odbcinst -q -d -n 'Apache Arrow Flight SQL ODBC Driver' | grep -ic '^Driver=')" -eq 1

# Uninstall: package-owned driver and sample DSN registrations are removed.
sudo dnf remove -y apache-arrow-flight-sql-odbc-driver
! odbcinst -q -d -n "Apache Arrow Flight SQL ODBC Driver"
! odbcinst -q -s -n "Apache Arrow Flight SQL ODBC DSN"
```

The RPM is intentionally separate from the full-Arrow RPM workflow. It does
not bundle Arrow shared libraries or a private CA bundle; RPM's automatic ELF
dependency discovery records the required system libraries, and the sample DSN
uses the system trust store. Edit `/etc/odbc.ini` with the target host, port,
and credentials before connecting. The package's driver name is prefixed with
`Apache` so it does not silently replace a separately installed Dremio driver.

### RPM through the full Arrow release packager

The repository has a separate full-Arrow RPM workflow, documented in
`dev/tasks/linux-packages/README.md`. It rebuilds Arrow from source on an
RPM-based container; it does not convert the already-built Ubuntu `.so` into an
RPM. The ODBC runtime subpackage was added after the 25.0.1 release, so the
25.0.1 release spec itself cannot produce an ODBC RPM without backporting that
spec/build change.

On a checkout that contains the ODBC RPM spec (including current `main`), run:

```bash
cd dev/tasks/linux-packages/apache-arrow
rake yum:build YUM_TARGETS=almalinux-9
```

The command requires Ruby, Docker, and the RPM-packaging container images. The
result is under
`yum/repositories/almalinux/9/x86_64/Packages/`. The ODBC runtime package is
named `apache-arrow<so_version>-flight-sql-odbc-libs` and is built alongside the
Arrow, Flight, and Flight SQL runtime packages it depends on. The RPM spec
registers the driver with `odbcinst` after installation. To make a 25.0.1 RPM,
backport the ODBC RPM spec/build changes onto the 25.0.1 source first, then run
the same `rake yum:build` command from that clean checkout. A standalone spec
can also package the existing `.so`, but it must declare the correct runtime
dependencies and `odbcinst` `%post`/`%postun` registration; it is not a native
RPM rebuild of the Ubuntu binary.

Create the stripped direct library, tar archive, smoke-test binary, and
`SHA256SUMS` in the Linux-only artifact directory:

```bash
linux_dir=cpp/src/arrow/flight/sql/odbc/install/linux
mkdir -p "${linux_dir}/artifacts"
docker run --rm --platform linux/amd64 \
  -v "$PWD:/arrow" -w /arrow \
  apache/arrow-dev:amd64-ubuntu-24.04-cpp \
  "${linux_dir}/package.sh" \
  /arrow/cpp/build/linux-odbc-validation/cpp/release/libarrow_flight_sql_odbc.so \
  "/arrow/${linux_dir}/artifacts"

cd "${linux_dir}/artifacts"
sha256sum --check SHA256SUMS
cd -
```

## Install and register with unixODBC

Ubuntu runtime prerequisites are the `unixodbc`, `odbcinst`, `libcurl4t64`, and
`ca-certificates` packages, plus the standard C/C++ runtime and the transitive
libraries reported by `ldd`. Verify the artifact before install:

```bash
sha256sum --check SHA256SUMS
tar -xzf apache-arrow-flight-sql-odbc-25.0.1-linux-x86_64.tar.gz
cd apache-arrow-flight-sql-odbc-25.0.1-linux-x86_64
ldd lib/libarrow_flight_sql_odbc.so
sudo ./install.sh
odbcinst -q -d -n 'Apache Arrow Flight SQL ODBC Driver'
```

`install.sh` defaults to `/opt/apache-arrow-flight-sql-odbc/25.0.1` and uses
`odbcinst` to register the absolute library path. Pass a different absolute
prefix as its sole argument if required. Re-running it at the same prefix is
idempotent. It refuses to overwrite a same-name registration at another path;
`uninstall.sh` likewise unregisters only the exact path it owns.

For a non-root, isolated validation, avoid changing `/etc/odbcinst.ini`:

```bash
package_root="$PWD/apache-arrow-flight-sql-odbc-25.0.1-linux-x86_64"
mkdir -p "$PWD/odbc-config"
sed "s|@DRIVER_PATH@|${package_root}/lib/libarrow_flight_sql_odbc.so|g" \
  "${package_root}/odbcinst.ini.in" >"$PWD/odbc-config/odbcinst.ini"
export ODBCSYSINI="$PWD/odbc-config"
export ODBCINSTINI=odbcinst.ini
odbcinst -q -d -n 'Apache Arrow Flight SQL ODBC Driver'
```

## Run the Dremio Cloud smoke test securely

The smoke test defaults to `data.eu.dremio.cloud:443`, enables TLS, verifies the
server certificate against the Linux system trust store, and executes
`SELECT 1`. For token authentication, it prefers a short-lived token from the
file named by `DREMIO_ODBC_TOKEN_FILE`. For user/password authentication, it
reads `DREMIO_ODBC_UID` and prefers the secret from
`DREMIO_ODBC_PASSWORD_FILE`. The connection string exists only in process
memory and is never printed.

Have the secret manager materialize a short-lived secret as a mode-0600 file,
then export only its path. Do not paste the secret into these commands.

```bash
export DREMIO_ODBC_TOKEN_FILE=/run/secrets/dremio_odbc_token
export DREMIO_ODBC_HOST=data.eu.dremio.cloud
export DREMIO_ODBC_PORT=443

./smoke/flight_sql_odbc_smoke_test
```

Expected evidence includes `connection: success`, `result: 1`, successful cursor,
statement, connection, and environment cleanup lines, and `smoke test: PASS`.
A successful compile or driver registration alone is not a successful remote
smoke test.

After the test, revoke the short-lived credential and remove the secret file
through the secret manager. Then clear the process environment:

```bash
unset DREMIO_ODBC_TOKEN_FILE DREMIO_ODBC_UID DREMIO_ODBC_PASSWORD_FILE \
  DREMIO_ODBC_HOST DREMIO_ODBC_PORT
```

## Uninstall and tear down

From the extracted package directory:

```bash
sudo ./uninstall.sh
if odbcinst -q -d -n 'Apache Arrow Flight SQL ODBC Driver'; then
  echo 'driver registration still exists' >&2
  exit 1
fi
```

Remove only the disposable Colima profile created for this validation:

```bash
colima stop arrow-odbc-linux
colima delete arrow-odbc-linux
```

If a cloud VM is substituted for Colima, record its instance ID, security group,
key, and volume IDs before use, then terminate the instance and delete only those
recorded resources. Confirm that no volume, elastic IP, or security group remains.
