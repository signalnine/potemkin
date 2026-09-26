# Boot PotemkinOS in qemu with the serial port as the console.
# Run with: bash tools/run-vm.sh [api|cuda] [extra kernel args...]
# llm=api defaults to a q27-server on the host (10.0.2.2 is the host's
# loopback under user networking). Pass api_url=/api_model=/api_key= to point
# it elsewhere; api_key on the cmdline is the design, but prefer a local server.
set -euo pipefail
cd "$(dirname "$0")/.."
flavor=${1:-api}
shift || true
Q=build/qemu
KERNEL=${KERNEL:-$(ls build/kernel/boot/vmlinuz-* | head -1)}
append="console=ttyS0 potemkin.tty=/dev/ttyS0 potemkin.net=${NET:-10.0.2.15/24,10.0.2.2,10.0.2.3} llm=$flavor"
if [[ $flavor == api ]]; then
  append+=" api_url=${API_URL:-http://10.0.2.2:8090/v1} api_model=${API_MODEL:-bonsai2-27b-t3-slim}"
fi
append+=" $*"
exec env LD_LIBRARY_PATH=$Q/usr/lib/x86_64-linux-gnu "$Q/usr/bin/qemu-system-x86_64" \
  -L "$Q/usr/share/qemu" -L "$Q/usr/share/seabios" -L "$Q/usr/lib/ipxe/qemu" \
  -enable-kvm -cpu host -m ${MEM:-4G} -smp 4 \
  -kernel "$KERNEL" -initrd "build/image-$flavor/initramfs.gz" -append "$append" \
  -drive file=build/disk-$flavor.img,if=virtio,format=raw \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -nographic -no-reboot
