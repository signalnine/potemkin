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
disk=build/disk-$flavor.img
extra=()
uplink="user,id=n0"
# NODE=N: one of several villages. Own disk and hostname, plus a second NIC on
# a private segment (10.10.0.1N) shared with the other nodes over UDP
# multicast, which needs no root.
if [[ -n ${NODE:-} ]]; then
  disk=build/disk-$flavor-node$NODE.img
  [[ -f $disk ]] || cp --sparse=always build/disk-$flavor.img "$disk"
  # Unattended villages get no route to the host's loopback services or the
  # internet: restrict=on, plus one forwarded port for the model API.
  # NET_OPEN=1 lifts this (and exposes everything on the host's 127.0.0.1).
  if [[ -z ${NET_OPEN:-} ]]; then
    uplink="user,id=n0,restrict=on,guestfwd=tcp:10.0.2.100:8090-tcp:127.0.0.1:${API_PORT:-8090}"
    append=${append//api_url=http:\/\/10.0.2.2:8090/api_url=http:\/\/10.0.2.100:8090}
  fi
  append+=" potemkin.hostname=node$NODE potemkin.net2=10.10.0.1$NODE/24"
  extra=(-device virtio-net-pci,netdev=n0,mac=52:54:00:00:00:1$NODE
         -netdev socket,id=n1,mcast=230.0.0.1:1234 -device virtio-net-pci,netdev=n1,mac=52:54:00:10:00:1$NODE)
else
  extra=(-device virtio-net-pci,netdev=n0)
fi
append+=" $*"
exec env LD_LIBRARY_PATH=$Q/usr/lib/x86_64-linux-gnu "$Q/usr/bin/qemu-system-x86_64" \
  -L "$Q/usr/share/qemu" -L "$Q/usr/share/seabios" -L "$Q/usr/lib/ipxe/qemu" \
  -enable-kvm -cpu host -m ${MEM:-4G} -smp 4 \
  -kernel "$KERNEL" -initrd "build/image-$flavor/initramfs.gz" -append "$append" \
  -drive file=$disk,if=virtio,format=raw \
  -netdev "$uplink" "${extra[@]}" \
  -nographic -no-reboot
