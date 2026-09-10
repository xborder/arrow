#!/usr/bin/env bash
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "Usage: build_rpm.sh <driver-library> <output-directory> [version]" >&2
  exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../../../../../../../../" && pwd)"
driver="$(realpath "$1")"
output_dir="$(realpath -m "$2")"
version="${3:-25.0.1}"
release="${RPM_RELEASE:-1}"

for command in file realpath rpmbuild sha256sum; do
  if ! command -v "${command}" >/dev/null 2>&1; then
    echo "required command is missing: ${command}" >&2
    exit 1
  fi
done

if [[ "$(uname -m)" != "x86_64" ]]; then
  echo "this RPM builder currently supports x86_64 only" >&2
  exit 1
fi
if [[ ! "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "version must have the form MAJOR.MINOR.PATCH: ${version}" >&2
  exit 1
fi
if [[ ! "${release}" =~ ^[0-9]+$ ]]; then
  echo "RPM_RELEASE must be numeric: ${release}" >&2
  exit 1
fi
if [[ ! -f "${driver}" ]]; then
  echo "driver library does not exist: ${driver}" >&2
  exit 1
fi
if ! file "${driver}" | grep -q 'ELF 64-bit.*x86-64'; then
  echo "driver is not a Linux x86_64 ELF shared library" >&2
  exit 1
fi

mkdir -p "${output_dir}"
staging="$(mktemp -d)"
trap 'rm -rf "${staging}"' EXIT
topdir="${staging}/rpmbuild"
mkdir -p "${topdir}"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

driver_file="libarrow_flight_sql_odbc.so.${version}"
driver_path="/opt/arrow-flight-sql-odbc-driver/lib64/libarrow_flight_sql_odbc.so"
cp "${driver}" "${topdir}/SOURCES/driver.so"
sed -e "s|@VERSION@|${version}|g" \
  -e "s|@DRIVER_PATH@|${driver_path}|g" \
  "${script_dir}/rpm/odbcinst.ini.in" >"${topdir}/SOURCES/odbcinst.ini"
sed "s|@VERSION@|${version}|g" \
  "${script_dir}/rpm/odbc.ini.in" >"${topdir}/SOURCES/odbc.ini"
for source in LICENSE.txt NOTICE.txt; do
  cp "${repo_root}/${source}" "${topdir}/SOURCES/${source}"
done
cp "${script_dir}/../../README.md" "${topdir}/SOURCES/README.md"
cp "${script_dir}/../../connection-options.md" "${topdir}/SOURCES/connection-options.md"
cp "${script_dir}/BUILDING.md" "${topdir}/SOURCES/BUILDING.md"
cp "${script_dir}/VALIDATION.md" "${topdir}/SOURCES/VALIDATION.md"
sed -e "s|@VERSION@|${version}|g" \
  -e "s|@RELEASE@|${release}|g" \
  "${script_dir}/rpm/arrow-flight-sql-odbc-driver.spec.in" \
  >"${topdir}/SPECS/apache-arrow-flight-sql-odbc-driver.spec"

rpmbuild --define "_topdir ${topdir}" \
  -bb "${topdir}/SPECS/apache-arrow-flight-sql-odbc-driver.spec"

rpm_path="${topdir}/RPMS/x86_64/apache-arrow-flight-sql-odbc-driver-${version}-${release}.$(uname -m).rpm"
if [[ ! -f "${rpm_path}" ]]; then
  rpm_path="$(find "${topdir}/RPMS" -type f -name '*.rpm' -print -quit)"
fi
if [[ -z "${rpm_path}" || ! -f "${rpm_path}" ]]; then
  echo "rpmbuild completed without producing an RPM" >&2
  exit 1
fi
destination="${output_dir}/$(basename "${rpm_path}")"
install -m 0644 "${rpm_path}" "${destination}"
(
  cd "${output_dir}"
  sha256sum "$(basename "${destination}")" >RPM-SHA256SUMS
)
echo "Built ${destination}"
