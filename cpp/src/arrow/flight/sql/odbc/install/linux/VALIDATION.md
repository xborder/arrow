# Apache Arrow Flight SQL ODBC 25.0.1 RPM validation

Validation date: 2026-09-10

Source commit: `beccec0d0c451b7aa3e4530416ac431b3c035c69`

Source tag: `apache-arrow-25.0.1`

## Outcome

| Check | Result |
|---|---|
| Exact 25.0.1 source and clean starting worktree | PASS |
| Upstream ODBC RPM commits cherry-picked onto 25.0.1 | PASS, no conflicts |
| Native Arrow YUM RPM build on Amazon Linux 2023 | PASS |
| Native ODBC RPM metadata, dependencies, file list, and checksum | PASS |
| RPM-set install and `%post` unixODBC registration | PASS |
| RPM driver dynamic load with no missing dependencies | PASS |
| RPM reinstall preserves one registration | PASS |
| RPM removal and `%postun` cleanup | PASS |
| Authenticated `SELECT 1` through the RPM-installed driver | PASS, 3/3 |
| AWS resources and temporary credential file removed | PASS |

## Environment

Validation used a fresh native x86_64 Amazon Linux 2023 EC2 VM in the AWS
Dremio Alliances account. The host had 16 vCPUs, 64 GiB RAM, 100 GiB encrypted
gp3 storage, Docker Engine, Ruby/Rake, Git, GCC, and unixODBC. Arrow's build
dependencies were installed inside the native `amazon-linux-2023` packager
container. The instance was terminated after validation; its temporary
security group and EC2 key pair were deleted, the root volume disappeared, and
the local temporary key and known-hosts files were removed.

## Backport and RPM build

The exact 25.0.1 tag predates ODBC RPM support. These upstream fixes were
cherry-picked in order onto that tag, with no conflicts:

1. `a1ec26fa34a5a24b260a48233942376cbd6b4ba1` (GH-47877): enable and split
   the ODBC runtime and development RPM packages.
2. `63fde363e2fc6f6cbfe94e99b3083ea013b25445` (GH-50518): install the generated
   `odbcinst` template and register/unregister the driver in RPM scripts.
3. `4dd7eb75eadf5129741888f261b6b2a74128fa11` (GH-50697): include ODBC
   documentation and correct installer behavior.

The patched checkout was archived at the stable-release Rake task's expected
path and built using the documented command:

```text
rake yum:build YUM_TARGETS=amazon-linux-2023
```

The output included:

```text
arrow2500-flight-sql-odbc-libs-25.0.1-1.amzn2023.x86_64.rpm
arrow-flight-sql-odbc-devel-25.0.1-1.amzn2023.x86_64.rpm
```

The checked-in runtime package is:

```text
arrow2500-flight-sql-odbc-libs-25.0.1-1.amzn2023.x86_64.rpm
SHA-256: 89ddb4d0d267cd332e0f545585bbf5798320caea452384214e933badc9dc425e
```

`rpm -qip`, `rpm -qpl`, and `rpm -qp --requires` confirmed the package
metadata, `/usr/lib64/libarrow_flight_sql_odbc.so.*`, generated
`/usr/share/arrow/flight/sql/odbc/arrow-flight-sql-odbc-template.ini`,
documentation, licenses, and dependency on the matching
`arrow2500-flight-sql-libs` package. The RPM uses Arrow's normal split-package
layout and dependency graph; it is intentionally not Dremio's standalone
`/opt` package.

## RPM lifecycle

The complete local RPM set was installed with DNF. The runtime package's
`%post` script registered exactly one `Apache Arrow Flight SQL ODBC Driver`
entry through `odbcinst`. The registered library loaded on native x86_64
Amazon Linux 2023 and `ldd` reported no missing dependencies. Reinstalling the
exact runtime RPM path preserved one registration. Removing the runtime,
optional debuginfo, and development packages removed the registration and the
runtime package-owned ODBC files through `%postun`.

## Query through the RPM-installed driver

After installing the RPM set, install the small build-only dependencies and
compile the checked-in smoke client. The client uses the RPM-created system
registration and defaults to `data.eu.dremio.cloud:443`:

```bash
sudo dnf install -y gcc-c++ unixODBC-devel
cd cpp/src/arrow/flight/sql/odbc/install/linux
./build_smoke_test.sh /tmp/flight_sql_odbc_rpm_smoke_test
secret_file="$(mktemp)"
chmod 600 "$secret_file"
trap 'rm -f "$secret_file" /tmp/flight_sql_odbc_rpm_smoke_test' EXIT
printf 'Dremio token: '
IFS= read -r -s DREMIO_ODBC_TOKEN
printf '\n'
printf '%s\n' "$DREMIO_ODBC_TOKEN" > "$secret_file"
unset DREMIO_ODBC_TOKEN
export DREMIO_ODBC_TOKEN_FILE="$secret_file"
export DREMIO_ODBC_HOST=data.eu.dremio.cloud
export DREMIO_ODBC_PORT=443
/tmp/flight_sql_odbc_rpm_smoke_test
```

The validation ran this sequence three times. Each run connected with TLS
certificate verification enabled, executed `SELECT 1`, returned `1`, closed the
cursor, freed the statement/connection/environment, and printed `smoke test:
PASS`. No token was placed in a repository file, command-line argument, ODBC
configuration, or artifact; the mode-0600 secret file was removed by the trap.

## Limitations

- This validates packaging, ABI loading, and one authenticated query, not
  performance, failover, or broad SQL conformance.
- The RPM artifact is built for Amazon Linux 2023 / glibc 2.34. Compatibility
  with another RPM distribution still requires validation on that distribution's
  oldest supported release.
