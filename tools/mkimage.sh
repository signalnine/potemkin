# Build the PotemkinOS initramfs and a persistent disk image.
# Run with: bash tools/mkimage.sh [api|cuda]
#
# The initramfs holds the trusted base and nothing else: init/rescue, q27-init,
# glibc, the tcc+musl sysroot, certificates, and (cuda) the NVIDIA driver
# pieces. No shell, no coreutils, no /bin.
set -euo pipefail
cd "$(dirname "$0")/.."
flavor=${1:-api}
# Which q27-init goes in: build/q27-init-$INIT, one of api (tools/build.sh
# api, no CUDA) or 12g/w8/full (tools/build.sh init with that PROFILE).
# api images default to api, cuda images to 12g.
INIT=${INIT:-$([[ $flavor == api ]] && echo api || echo 12g)}
out=build/image-$flavor
stage=$out/root
rm -rf "$out"
mkdir -p "$stage"/{sbin,usr/bin,usr/lib,lib64,lib/x86_64-linux-gnu,etc/ssl/certs,proc,sys,dev,tmp,run}

install -m 0755 build/init "$stage/sbin/init"
ln "$stage/sbin/init" "$stage/sbin/rescue"
ln -s sbin/init "$stage/init"  # the kernel runs /init from an initramfs
[[ -f build/q27-init-$INIT ]] || { echo "mkimage.sh: build/q27-init-$INIT missing (tools/build.sh api|init)" >&2; exit 1; }
install -m 0755 "build/q27-init-$INIT" "$stage/usr/bin/q27-init"
strip "$stage/usr/bin/q27-init" "$stage/sbin/init" 2>/dev/null || true

# glibc: what q27-init, tcc and libcuda load (docs/link-audit.md).
install -m 0755 /lib64/ld-linux-x86-64.so.2 "$stage/lib64/"
for l in libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1; do
  install -m 0644 "/lib/x86_64-linux-gnu/$l" "$stage/lib/x86_64-linux-gnu/"
done

# The compiler the model gets: tcc + musl, static output.
mkdir -p "$stage/usr/lib/potemkin"
cp -a build/sysroot/usr "$stage/usr/lib/potemkin/"
rm -rf "$stage/usr/lib/potemkin/usr/share"

if [[ $flavor == api ]]; then
  cp -L /etc/ssl/certs/ca-certificates.crt "$stage/etc/ssl/certs/"
fi

if [[ $flavor == cuda ]]; then
  kver=${KVER:-$(uname -r)}
  drv=$(modinfo -k "$kver" -F version nvidia)
  mkdir -p "$stage/lib/modules/potemkin" "$stage/lib/firmware/nvidia/$drv"
  for m in nvidia nvidia-uvm; do
    src=$(modinfo -k "$kver" -n "$m")
    case $src in
      *.zst) zstd -dq "$src" -o "$stage/lib/modules/potemkin/$m.ko" ;;
      *) install -m 0644 "$src" "$stage/lib/modules/potemkin/$m.ko" ;;
    esac
    echo "$m.ko" >> "$stage/lib/modules/potemkin/load"
  done
  cp /lib/firmware/nvidia/$drv/gsp_ga10x.bin "$stage/lib/firmware/nvidia/$drv/"
  cp -L /lib/x86_64-linux-gnu/libcuda.so.1 "$stage/lib/x86_64-linux-gnu/libcuda.so.1"
fi

( cd "$stage" && find . -print0 | sort -z | cpio --null -o -H newc --quiet ) | gzip -6 > "$out/initramfs.gz"
echo "initramfs: $out/initramfs.gz ($(du -h "$out/initramfs.gz" | cut -f1))"
echo "contents:"
( cd "$stage" && find . -type f -o -type l | grep -v '^./usr/lib/potemkin/' | sort | sed 's/^\./  /' )
echo "  /usr/lib/potemkin/... ($(find "$stage/usr/lib/potemkin" -type f | wc -l) files: tcc + musl)"

# Persistent disk for the VM (run-vm.sh): the trees that survive reboots.
# Created once, kept after. A cuda image boots on bare metal and uses a real
# partition (potemkin.disk=), so it gets none.
[[ $flavor == cuda ]] && { echo "disk: none (bare metal uses potemkin.disk=)"; exit 0; }
disk=build/disk-$flavor.img
if [[ ! -f $disk ]]; then
  mkdir -p "$out/disk"/{data,state,generated/bin,store,intent,cache,snapshots,models}
  truncate -s 4G "$disk"
  mkfs.ext4 -q -F -L potemkin -d "$out/disk" "$disk"
  echo "disk: $disk (new, 4G sparse)"
else
  echo "disk: $disk (kept)"
fi
