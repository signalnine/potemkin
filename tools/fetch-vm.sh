# Fetch QEMU and a VM kernel from the distro archive into build/qemu and
# build/kernel. Like fetch-toolchain.sh: apt-get download needs no root and
# runs no package scripts; dpkg-deb -x only unpacks files.
# Run with: bash tools/fetch-vm.sh      (Debian/Ubuntu x86_64)
#
# Why download a kernel: /boot/vmlinuz-* is root-only on Ubuntu, and the api
# image loads no modules, so the kernel needs virtio (disk, net) and ext4
# built in. Ubuntu's generic kernels have that; this script checks.
# KERNEL_PKG=linux-image-<version>-generic picks a specific one.
set -euo pipefail
cd "$(dirname "$0")/.."
DEST=${DEST:-build}
debs=third_party/debs/vm
mkdir -p "$debs" "$DEST/qemu" "$DEST/kernel"

dl() { (cd "$debs" && apt-get download "$@" 2>/dev/null); }

# QEMU plus the firmware and the libraries a desktop may not have. Package
# names drift between releases, so each is tried on its own.
dl qemu-system-x86 qemu-system-common qemu-system-data seabios ipxe-qemu
for p in libfdt1 libpmem1 libslirp0 librdmacm1t64 librdmacm1 libibverbs1 libndctl6 libdaxctl1 \
         libcapstone5 libcapstone4 libvdeplug2t64 libvdeplug2 liburing2 libaio1t64 libaio1; do
  dl "$p" || true
done
for d in "$debs"/*.deb; do dpkg-deb -x "$d" "$DEST/qemu"; done
missing=$(LD_LIBRARY_PATH=$DEST/qemu/usr/lib/x86_64-linux-gnu ldd "$DEST/qemu/usr/bin/qemu-system-x86_64" | awk '/not found/{print $1}')
if [[ -n $missing ]]; then
  echo "fetch-vm.sh: qemu still misses: $missing" >&2
  echo "  find the package that ships each one and add it with: (cd $debs && apt-get download <pkg>)" >&2
  exit 1
fi
echo "qemu: $("$DEST/qemu/usr/bin/qemu-system-x86_64" --version 2>/dev/null | head -1 || LD_LIBRARY_PATH=$DEST/qemu/usr/lib/x86_64-linux-gnu "$DEST/qemu/usr/bin/qemu-system-x86_64" --version | head -1)"

# The kernel: the concrete package behind the distro's generic metapackage.
pkg=${KERNEL_PKG:-}
if [[ -z $pkg ]]; then
  . /etc/os-release
  for meta in "linux-image-generic-hwe-${VERSION_ID:-}" linux-image-generic linux-image-amd64; do
    pkg=$(apt-cache depends "$meta" 2>/dev/null | awk '/Depends: linux-image-[0-9]/{print $2; exit}')
    [[ -n $pkg ]] && break
  done
fi
[[ -n $pkg ]] || { echo "fetch-vm.sh: no kernel package found; set KERNEL_PKG" >&2; exit 1; }
(cd "$debs" && apt-get download "$pkg") || { echo "fetch-vm.sh: cannot download $pkg; set KERNEL_PKG to one apt can fetch" >&2; exit 1; }
dpkg-deb -x "$debs/$pkg"_*.deb "$DEST/kernel"
# The config ships in the matching modules package; keep only that file.
mods=${pkg/linux-image-/linux-modules-}
if (cd "$debs" && apt-get download "$mods" 2>/dev/null); then
  tmp=$(mktemp -d)
  dpkg-deb -x "$debs/$mods"_*.deb "$tmp"
  cp "$tmp"/boot/config-* "$DEST/kernel/boot/" 2>/dev/null || true
  rm -rf "$tmp" "$debs/$mods"_*.deb
fi
cfg=$(ls "$DEST"/kernel/boot/config-* 2>/dev/null | head -1)
[[ -n $cfg ]] || { echo "fetch-vm.sh: no config for $pkg to check; if the VM finds no disk or network, pick an Ubuntu generic kernel via KERNEL_PKG" >&2; exit 0; }
for opt in CONFIG_VIRTIO_BLK CONFIG_VIRTIO_NET CONFIG_VIRTIO_PCI CONFIG_EXT4_FS CONFIG_DEVTMPFS; do
  grep -q "^$opt=y" "$cfg" || { echo "fetch-vm.sh: $pkg has $opt as a module or not at all; the api image needs it built in (try an Ubuntu generic kernel via KERNEL_PKG)" >&2; exit 1; }
done
echo "kernel: $(ls "$DEST"/kernel/boot/vmlinuz-*) ($pkg)"
