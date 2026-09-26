#!/usr/bin/env bash
# Fetch tcc + musl from the distro archive and unpack into build/sysroot.
# apt-get download needs no root and runs no maintainer scripts; dpkg-deb -x
# only unpacks files.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party/debs build/sysroot
( cd third_party/debs && apt-get download tcc musl musl-dev )
for d in third_party/debs/*.deb; do dpkg-deb -x "$d" build/sysroot; done
echo "sysroot ready: build/sysroot"
