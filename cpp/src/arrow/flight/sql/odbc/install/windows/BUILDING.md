# Building the x64 Windows installer

This produces the unsigned Flight SQL ODBC MSI for commit
`beccec0d0c451b7aa3e4530416ac431b3c035c69` (version `25.0.1`).  Use a
Windows Server 2022 x64 machine with Visual Studio 2022 C++ Build Tools, Git
for Windows (including Git Bash), CMake, Ninja, and WiX 6 installed.

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
