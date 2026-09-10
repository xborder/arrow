# Apache Arrow Flight SQL ODBC 25.0.1 Linux validation

Validation date: 2026-09-10

Source commit: `beccec0d0c451b7aa3e4530416ac431b3c035c69`

Source tag: `apache-arrow-25.0.1`

## Outcome

| Check | Result |
|---|---|
| Exact source and clean starting worktree | PASS |
| Repository-defined Ubuntu 24.04 amd64 build | PASS |
| x86_64 ELF and exported ODBC entry points | PASS |
| Dynamic dependency resolution | PASS |
| TLS CA-chain and hostname verification | PASS |
| Isolated unixODBC registration | PASS |
| Authenticated `SELECT 1` against Dremio Cloud | PASS, 3/3 |
| Cursor, statement, connection, and environment cleanup | PASS, 3/3 |
| Relocatable tar archive, checksum, install, load, and uninstall | PASS |
| Upstream ODBC RPM commits cherry-picked onto 25.0.1 | PASS, no conflicts |
| Native Arrow YUM RPM build on Amazon Linux 2023 | PASS |
| Native ODBC RPM metadata, dependencies, and file list | PASS |
| RPM install, reinstall, unixODBC registration, dynamic load, and removal | PASS |
| AWS validation resources and credential file removal | PASS |

## Environment

The tar/driver validation used the same Ubuntu version, architecture,
bundled-dependency mode, and disabled shared dependency linkage as Arrow's
`odbc-linux` CI job and `ubuntu-cpp-odbc` Compose service. CMake reported Arrow
25.0.1, x86_64, and the Release configuration. The build completed the
`install` target successfully.

- Container image:
  `apache/arrow-dev@sha256:a887c3bfb5262539c9046d414a8dfa9390c65358019e760128e8104e86041b36`
- Container architecture: `x86_64` / Debian architecture `amd64`.
- Distribution: Ubuntu 24.04.4 LTS.
- Toolchain: GCC/G++ 13.3.0, CMake 3.28.3, Ninja 1.11.1.
- Runtime: glibc 2.39 and unixODBC 2.3.12.

The RPM validation used a disposable native x86_64 Amazon Linux 2023 VM in
the AWS Dremio Alliances account. The host had Docker Engine, Ruby, Git, and
CA certificates; Arrow's build dependencies were installed by the native
`amazon-linux-2023` YUM packager container. The VM had at least 16 vCPUs,
64 GiB RAM, and 100 GiB of encrypted gp3 storage. It was terminated after the
tests, its temporary security group and EC2 key pair were deleted, and no
temporary credential file remained locally.

## Build and driver evidence

The unstripped result was 60.4 MiB and identified as:

```text
ELF 64-bit LSB shared object, x86-64
Machine: Advanced Micro Devices X86-64
Type: DYN (Shared object file)
```

The driver exported the expected Unix wide-character entry points, including
`SQLConnectW`, `SQLDriverConnectW`, and `SQLExecDirectW`, plus common entry
points such as `SQLDisconnect`, `SQLFetch`, and `SQLCloseCursor`.

Direct dynamic dependencies were `libcurl.so.4`, `libodbcinst.so.2`,
`libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6`, and the x86_64
loader. `ldd` resolved those and every transitive dependency. No dynamic Arrow,
Flight, Flight SQL, gRPC, or Protobuf library was required by the relocatable
driver build.

An independent OpenSSL preflight against `data.eu.dremio.cloud:443`, with SNI,
`-verify_hostname`, and `-verify_return_error`, reported `Verification: OK` and
`Verify return code: 0 (ok)`. The compiled smoke client then performed three
independent authenticated cycles. Every cycle reported connection success,
`SELECT 1`, result `1`, successful cursor close, statement free, disconnect,
connection free, environment free, and `smoke test: PASS`.

The driver was registered in an isolated mode-0600 `odbcinst.ini`; no host or
container system ODBC configuration was modified. The relocatable archive
passed checksum verification, extraction, installation at the default prefix,
repeat installation with `UsageCount=1`, dynamic loading, documentation
installation, unregistration, and exact-prefix file removal.

## Backport and native RPM evidence

The exact 25.0.1 tag predates ODBC RPM support. The following upstream fixes
were cherry-picked in order onto that tag, with no conflicts:

1. `a1ec26fa34a5a24b260a48233942376cbd6b4ba1` (GH-47877): enable and split
   the ODBC runtime and development RPM packages.
2. `63fde363e2fc6f6cbfe94e99b3083ea013b25445` (GH-50518): install the generated
   `odbcinst` template and register/unregister the driver in RPM scripts.
3. `4dd7eb75eadf5129741888f261b6b2a74128fa11` (GH-50697): include ODBC
   documentation and correct installer behavior.

The patched checkout was archived at the path expected by Arrow's stable
release Rake task, then built with the repository's original command:

```text
rake yum:build YUM_TARGETS=amazon-linux-2023
```

The build produced the upstream-native split packages:

```text
arrow2500-flight-sql-odbc-libs-25.0.1-*.x86_64.rpm
arrow-flight-sql-odbc-devel-25.0.1-*.x86_64.rpm
```

The runtime package installs `libarrow_flight_sql_odbc.so.*` under
`/usr/lib64`, installs the generated template under
`/usr/share/arrow/flight/sql/odbc/`, and requires the matching
`arrow2500-flight-sql-libs` package. It is therefore intentionally different
from Dremio's standalone `/opt` package: the native Arrow RPM is part of the
Arrow RPM repository and uses the normal Arrow runtime/development dependency
graph. Package metadata, file lists, and dependencies were inspected with
`rpm -qip`, `rpm -qpl`, and `rpm -qp --requires`.

The complete RPM set from that repository was installed with DNF. Validation
then confirmed the ODBC runtime package was installed, the RPM `%post` script
registered exactly one `Apache Arrow Flight SQL ODBC Driver` entry through
`odbcinst`, the shared library loaded with `ldd` without missing dependencies,
and `dnf reinstall` preserved the single registration. Removing the ODBC
runtime and development packages removed the registration and all package-owned
ODBC files through `%postun`.

The checked-in RPM artifact and `RPM-SHA256SUMS` correspond to this native
packager output:

```text
arrow2500-flight-sql-odbc-libs-25.0.1-1.amzn2023.x86_64.rpm
SHA-256: 89ddb4d0d267cd332e0f545585bbf5798320caea452384214e933badc9dc425e
```

The tar checksum file remains independent and covers the Ubuntu/glibc
relocatable artifact.

## Installer and credential safety

The Dremio token was never placed in a repository file, ODBC configuration,
command-line argument, or artifact. It was read only from a root-owned
mode-0600 temporary secret file, diagnostic output was redacted, and a shell
trap deleted the file immediately after the live attempts. A follow-up
existence check passed. The token should be revoked after validation because it
was supplied interactively.

## Limitations

- Functional and ABI behavior of the tar artifact was validated under
  translated x86_64 container execution; the RPM build and lifecycle were
  validated on native x86_64 Amazon Linux 2023.
- This is not a performance, load, failover, or broad SQL conformance test.
- The tar artifact is an Ubuntu 24.04 / glibc 2.39 build. The RPM artifact is
  an Amazon Linux 2023 / glibc 2.34 build; compatibility with another RPM
  distribution still requires validation on that distribution's oldest
  supported release.
