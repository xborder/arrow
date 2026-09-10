# Apache Arrow Flight SQL ODBC 25.0.1: Linux build, RPM package, and install

This runbook builds the Apache Arrow Flight SQL ODBC driver from the upstream
25.0.1 source, backports the upstream RPM fixes, and uses Arrow's native YUM
packager. The result is the normal split Arrow RPM set, including the ODBC
runtime and development packages. No credential is written to source,
configuration, shell history, logs, or artifacts.

Run the commands below from the root of a fresh Arrow checkout.

## Upstream RPM support

The generic RPM packager is documented in
`dev/tasks/linux-packages/README.md`. ODBC RPM support was added after the
25.0.1 release and is supplied by these upstream commits, applied in order:

1. `a1ec26fa34a5a24b260a48233942376cbd6b4ba1` (GH-47877): build and split the
   ODBC runtime and development packages.
2. `63fde363e2fc6f6cbfe94e99b3083ea013b25445` (GH-50518): generate the
   `odbcinst` template and register/unregister the driver in RPM scripts.
3. `4dd7eb75eadf5129741888f261b6b2a74128fa11` (GH-50697): include ODBC
   documentation and fix installer behavior.

The backport was tested against the `apache-arrow-25.0.1` tag with no
cherry-pick conflicts. The package is not Dremio's standalone `/opt` package:
it uses the upstream Arrow split-package layout under `/usr/lib64` and depends
on the matching Arrow Flight and Flight SQL runtime packages.

## Requirements

The host needs Git, Ruby, Docker Engine, and CA certificates. Arrow's RPM
build dependencies are installed inside the target distribution container. Use
an x86_64 RPM host that is no newer than the distributions you intend to
support; do not put the Ubuntu validation binary into an RPM for an older
distribution. Allow at least 30 GiB of free disk and 12 GiB of RAM.

For Amazon Linux 2023, the host setup used for validation was:

```bash
sudo dnf install -y git ruby docker ca-certificates
sudo systemctl enable --now docker
sudo usermod -aG docker "$USER"
# Start a new login session after adding the user to the docker group.
```

## Prepare a patched 25.0.1 source checkout

Keep the packaging checkout and the patched source checkout separate. This is
important because the stable release Rake task otherwise downloads the
unmodified 25.0.1 archive.

```bash
packaging_root="$PWD/arrow-25.0.1-rpm"
git clone https://github.com/apache/arrow.git "$packaging_root"
git -C "$packaging_root" switch --detach apache-arrow-25.0.1
git -C "$packaging_root" switch -c backport-25.0.1-rpm
git -C "$packaging_root" cherry-pick \
  a1ec26fa34a5a24b260a48233942376cbd6b4ba1 \
  63fde363e2fc6f6cbfe94e99b3083ea013b25445 \
  4dd7eb75eadf5129741888f261b6b2a74128fa11
git -C "$packaging_root" status --short
```

Create the source archive where the YUM task expects it. Because this file
already exists, the stable-release task uses the patched archive instead of
downloading the public 25.0.1 tarball:

```bash
git -C "$packaging_root" archive --format=tar.gz \
  --prefix=apache-arrow-25.0.1/ \
  -o "$packaging_root/dev/tasks/linux-packages/apache-arrow/apache-arrow-25.0.1.tar.gz" \
  HEAD
```

## Build the native Arrow RPMs

Run the original packager for the target distribution. The default command
builds all supported targets; the explicit target keeps the validation
reproducible:

```bash
cd "$packaging_root/dev/tasks/linux-packages/apache-arrow"
rake yum:build YUM_TARGETS=amazon-linux-2023
rpm_dir="$PWD/yum/repositories/amazon-linux/2023/x86_64/Packages"
find "$rpm_dir" -maxdepth 1 -type f -name '*flight-sql-odbc*.rpm' -print
```

The relevant packages are named `arrow2500-flight-sql-odbc-libs` (runtime) and
`arrow-flight-sql-odbc-devel` (headers and CMake files). The runtime package
contains `libarrow_flight_sql_odbc.so.*`, requires the matching
`arrow2500-flight-sql-libs`, and installs the generated
`arrow-flight-sql-odbc-template.ini` used for unixODBC registration.

Inspect the package before installation:

```bash
odbc_rpm="$(find "$rpm_dir" -maxdepth 1 \
  -name 'arrow2500-flight-sql-odbc-libs-25.0.1-*.rpm' -print -quit)"
test -n "$odbc_rpm"
rpm -qip "$odbc_rpm"
rpm -qpl "$odbc_rpm"
rpm -qp --requires "$odbc_rpm"
```

## Install, upgrade, load, and remove

Install the local RPM set together so DNF can resolve the matching Arrow
runtime packages produced by the same build:

```bash
sudo dnf install -y "$rpm_dir"/*.rpm
rpm -q arrow2500-flight-sql-odbc-libs
odbcinst -q -d -n 'Apache Arrow Flight SQL ODBC Driver'
driver="$(rpm -ql arrow2500-flight-sql-odbc-libs | \
  grep '/libarrow_flight_sql_odbc.so\.' | head -1)"
test -n "$driver"
ldd "$driver" | grep 'not found' && exit 1 || true
```

The RPM `%post` script registers the driver through the generated template.
Reinstalling must preserve exactly one registration:

```bash
sudo dnf reinstall -y "$odbc_rpm"
test "$(odbcinst -q -d -n 'Apache Arrow Flight SQL ODBC Driver' \
  | grep -ic '^Driver=')" -eq 1
```

Remove the runtime and development packages and verify `%postun` removed the
registration and package-owned files:

```bash
sudo dnf remove -y arrow2500-flight-sql-odbc-libs \
  arrow2500-flight-sql-odbc-libs-debuginfo arrow-flight-sql-odbc-devel
! grep -q 'Apache Arrow Flight SQL ODBC Driver' /etc/odbcinst.ini \
  /etc/odbcinst.ini.rpmsave 2>/dev/null
! test -e "$driver"
```

The package uses the system CA store and standard Arrow RPM dependencies. Set
the host, port, and authentication properties in the normal ODBC connection
configuration before making a remote query. Never place a Dremio token in a
repository file, command-line argument, or RPM.

## Existing relocatable tar artifact

The checked-in Ubuntu 24.04 x86_64 tar artifact remains available under
`install/linux/artifacts/`. Verify it independently with:

```bash
cd cpp/src/arrow/flight/sql/odbc/install/linux/artifacts
sha256sum --check SHA256SUMS
```

The tar artifact is an Ubuntu/glibc build; the RPM must always be rebuilt by
the native Arrow YUM packager on the oldest supported RPM distribution.
