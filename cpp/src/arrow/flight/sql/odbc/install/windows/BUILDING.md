# Building the x64 Windows installer

This produces the unsigned Flight SQL ODBC MSI for commit
`beccec0d0c451b7aa3e4530416ac431b3c035c69` (version `25.0.1`).  Use a
Windows Server 2022 x64 machine with Visual Studio 2022 C++ Build Tools, Git
for Windows (including Git Bash), CMake, Ninja, and WiX 6 installed.

## AWS build host

Use the Dremio Alliances AWS account (`812725540388`) only.  The build host
used for this release was a disposable `c6i.2xlarge` Windows Server 2022
instance.  It needs a subnet with outbound Internet access so it can download
the build tools and dependencies.  Use an instance profile that permits AWS
Systems Manager (SSM) core functionality and `s3:PutObject` to the approved
artifact prefix; this avoids opening an inbound RDP port or distributing a key
pair.

From a workstation with an AWS CLI profile that targets that account, select a
region, subnet, security group (outbound HTTPS only is sufficient), and an SSM
instance-profile name.  The following creates a 100 GiB ephemeral build host
and records its ID.  Replace the placeholder values before running it.

```bash
export AWS_PROFILE=<alliances-profile>
export AWS_REGION=<region>
export SUBNET_ID=<subnet-id>
export SECURITY_GROUP_ID=<security-group-id>
export INSTANCE_PROFILE=<ssm-instance-profile-name>

aws sts get-caller-identity
# Confirm that Account is 812725540388 before continuing.
export AMI_ID="$(aws ssm get-parameter \
  --name /aws/service/ami-windows-latest/Windows_Server-2022-English-Full-Base \
  --query 'Parameter.Value' --output text)"
export INSTANCE_ID="$(aws ec2 run-instances \
  --image-id "$AMI_ID" --instance-type c6i.2xlarge \
  --subnet-id "$SUBNET_ID" --security-group-ids "$SECURITY_GROUP_ID" \
  --iam-instance-profile Name="$INSTANCE_PROFILE" \
  --block-device-mappings 'DeviceName=/dev/sda1,Ebs={VolumeSize=100,VolumeType=gp3,DeleteOnTermination=true}' \
  --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=arrow-odbc-windows-build},{Key=Purpose,Value=ephemeral-build},{Key=Owner,Value=<owner>} ]' \
  --query 'Instances[0].InstanceId' --output text)"
echo "$INSTANCE_ID"
aws ec2 wait instance-status-ok --instance-ids "$INSTANCE_ID"
```

Connect through Session Manager once the SSM agent is online:

```bash
aws ssm start-session --target "$INSTANCE_ID"
```

In the administrator PowerShell session, install the prerequisites.  Reboot
when the Visual Studio installer requests it, reconnect with SSM, and rerun
any command that did not complete.  Verify `git`, `cmake`, `ninja`, and `wix`
are available before proceeding to the build steps below.

```powershell
Set-ExecutionPolicy Bypass -Scope Process -Force
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072
Invoke-Expression ((New-Object Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
choco install -y git cmake ninja awscli
choco install -y wixtoolset --version=6.0.2
choco install -y visualstudio2022buildtools --package-parameters "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --norestart"
```

The build needs substantial temporary space and may take several hours on its
first run because vcpkg builds dependencies.  Keep the generated MSI outside
the instance before teardown, for example by uploading it to the approved
private artifact location:

```powershell
Get-FileHash C:\src\arrow\build\cpp\Apache-Arrow-Flight-SQL-ODBC-25.0.1-win64.msi -Algorithm SHA256
aws s3 cp C:\src\arrow\build\cpp\Apache-Arrow-Flight-SQL-ODBC-25.0.1-win64.msi `
  s3://dremio-alliances/codex-artifacts/arrow-odbc/25.0.1/beccec0d0c451b7aa3e4530416ac431b3c035c69/ `
  --region us-west-2
```

After confirming the upload and recording the SHA-256, terminate the host;
do not stop it and leave it allocated:

```bash
aws s3api head-object --bucket dremio-alliances \
  --key codex-artifacts/arrow-odbc/25.0.1/beccec0d0c451b7aa3e4530416ac431b3c035c69/Apache-Arrow-Flight-SQL-ODBC-25.0.1-win64.msi \
  --region us-west-2
aws ec2 terminate-instances --instance-ids "$INSTANCE_ID"
aws ec2 wait instance-terminated --instance-ids "$INSTANCE_ID"
```

`DeleteOnTermination=true` removes the 100 GiB root volume.  Delete any
dedicated security group, EBS snapshots, or temporary S3 artifact only when
they were created for this build and are no longer needed; never delete a
shared network resource or an approved retained release artifact.

## Build and package

1. Clone recursively and check out the exact source revision:

   ```powershell
   git clone --recursive https://github.com/apache/arrow.git C:\src\arrow
   Set-Location C:\src\arrow
   git checkout beccec0d0c451b7aa3e4530416ac431b3c035c69
   git submodule update --init --recursive
   ```

2. Clone and bootstrap vcpkg, then open a **Developer Command Prompt for VS
   2022** (or call `vcvarsall.bat x64` from `cmd.exe`) before starting Git Bash:

   ```powershell
   git clone https://github.com/microsoft/vcpkg.git C:\src\vcpkg
   C:\src\vcpkg\bootstrap-vcpkg.bat
   call "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
   ```

3. From Git Bash, configure and build the Release installation.  The `ARROW_*`
   values mirror Arrow's Windows ODBC packaging workflow:

   ```bash
   export VCPKG_ROOT=/c/src/vcpkg
   export ARROW_BUILD_SHARED=ON ARROW_BUILD_STATIC=OFF ARROW_BUILD_TESTS=OFF
   export ARROW_USE_CCACHE=OFF
   export ARROW_BUILD_TYPE=release ARROW_CSV=OFF ARROW_DEPENDENCY_SOURCE=VCPKG
   export ARROW_FLIGHT_SQL_ODBC=ON ARROW_FLIGHT_SQL_ODBC_INSTALLER=ON
   export ARROW_HOME=/usr CMAKE_GENERATOR=Ninja VCPKG_DEFAULT_TRIPLET=x64-windows
   ci/scripts/cpp_build.sh "$(pwd)" "$(pwd)/build"
   ```

4. On a clean MSVC installation, CMake 4.2 may stop at the final install step
   because its runtime dependency scan cannot find `VCRUNTIME140_1.dll`.  The
   driver has already been built at this point.  Copy that x64 runtime beside
   the Release DLL, rerun the install, and put WiX on `PATH` before packaging:

   ```powershell
   Set-Location C:\src\arrow\build\cpp
   $runtime = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC' `
     -Recurse -Filter VCRUNTIME140_1.dll | Where-Object FullName -match '\\x64\\' | Select-Object -First 1
   Copy-Item $runtime.FullName .\release\ -Force
   cmake --build . --target install
   $env:PATH = 'C:\Program Files\WiX Toolset v6.0\bin;' + $env:PATH
   cpack
   ```

The resulting installer is
`Apache-Arrow-Flight-SQL-ODBC-25.0.1-win64.msi` in `build\cpp`.  It contains
the x64 driver DLL and required Arrow runtime DLLs.  Sign the DLL before
packaging and sign the MSI afterwards only when authorized code-signing
credentials are available.

## Windows smoke-test application

`odbc_smoke_test.cc` is a minimal x64 ODBC client that connects using a
connection string and runs `SELECT 1 AS odbc_smoke_test`.  On the build host,
open an x64 Developer Command Prompt for Visual Studio 2022 and compile it:

```cmd
cl /std:c++17 /EHsc /W4 odbc_smoke_test.cc /link odbc32.lib /out:odbc_smoke_test.exe
```

After installing the MSI, first confirm that its ODBC registration includes
non-empty `Driver` and `Setup` paths.  If either is empty, do not accept the
installer: the Windows driver manager will return `IM002` before it loads the
DLL.

```powershell
Get-ItemProperty 'HKLM:\SOFTWARE\ODBC\ODBCINST.INI\Apache Arrow Flight SQL ODBC Driver' |
  Select-Object Driver, Setup, DriverODBCVer
```

Run it with a Dremio Flight SQL endpoint and a short-lived test credential. Do
not put real passwords or tokens in this repository or command history.  For a
TLS endpoint using a personal-access token, the connection string shape is:

```cmd
odbc_smoke_test.exe "driver={Apache Arrow Flight SQL ODBC Driver};host=<dremio-host>;port=32010;token=<short-lived-token>;useEncryption=true;useWideChar=true;"
```

For a Dremio deployment with a private CA, add
`trustedCerts=C:\path\to\ca.pem;useSystemTrustStore=false`.  For a controlled
non-production plaintext endpoint only, use `useEncryption=false`.  A passing
test prints `Query succeeded; odbc_smoke_test=1`.
