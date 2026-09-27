# Boot PotemkinOS in qemu with the serial port as the console.
# Run with: bash tools/run-vm.sh [api|cuda] [extra kernel args...]
#
# llm=api settings, all from the environment:
#   API_URL    OpenAI-compatible base URL as seen from THIS machine
#              (default http://127.0.0.1:8090/v1, a q27-server on the host;
#              e.g. https://openrouter.ai/api/v1)
#   API_MODEL  model name the endpoint expects (e.g. qwen/qwen3.6-27b)
#   API_KEY    bearer token; it goes on the kernel cmdline, which is the joke.
#              The harness scrubs it from everything the model sees, and the
#              boot is made quiet so the kernel does not print it either.
#
# The VM's uplink reaches the API endpoint and nothing else: no internet, no
# DNS, no services on this machine's loopback (an unattended village will
# scan them). NET_OPEN=1 gives it the ordinary QEMU user network instead.
#
# NODE=N boots village N of an oblast: its own disk and hostname, plus a second
# NIC on a private LAN (10.10.0.1N) shared with the other villages.
set -euo pipefail
cd "$(dirname "$0")/.."
flavor=${1:-api}
shift || true
Q=build/qemu
KERNEL=${KERNEL:-$(ls build/kernel/boot/vmlinuz-* | head -1)}
append="console=ttyS0 potemkin.tty=/dev/ttyS0 potemkin.net=${NET:-10.0.2.15/24,10.0.2.2,10.0.2.3} llm=$flavor"

uplink="user,id=n0"
if [[ $flavor == api ]]; then
  url=${API_URL:-http://127.0.0.1:8090/v1}
  scheme=${url%%://*}
  rest=${url#*://}
  hostport=${rest%%/*}
  path=${rest#"$hostport"}
  host=${hostport%:*}
  port=${hostport##*:}
  [[ $hostport == *:* ]] || port=$([[ $scheme == https ]] && echo 443 || echo 80)
  local_api=0
  [[ $host == 127.0.0.1 || $host == localhost ]] && local_api=1
  if [[ -n ${NET_OPEN:-} ]]; then
    # 10.0.2.2 is this machine's loopback under QEMU user networking.
    (( local_api )) && host=10.0.2.2
    guest_url="$scheme://$host:$port$path"
  else
    # One forwarded endpoint; cmd: runs a relay per connection (tcp: would
    # connect once, at startup). A remote host keeps its name inside the VM,
    # mapped to the forward address, so TLS still checks the real certificate.
    target=$host
    (( local_api )) && target=127.0.0.1
    uplink="user,id=n0,restrict=on,guestfwd=tcp:10.0.2.100:$port-cmd:nc $target $port"
    if (( local_api )); then
      guest_url="$scheme://10.0.2.100:$port$path"
    else
      guest_url="$scheme://$host:$port$path"
      append+=" potemkin.hosts=$host=10.0.2.100"
    fi
  fi
  append+=" api_url=$guest_url api_model=${API_MODEL:-local}"
  if [[ -n ${API_KEY:-} ]]; then
    append+=" api_key=$API_KEY quiet"
  fi
fi

disk=build/disk-$flavor.img
if [[ -n ${NODE:-} ]]; then
  disk=build/disk-$flavor-node$NODE.img
  [[ -f $disk ]] || cp --sparse=always build/disk-$flavor.img "$disk"
  append+=" potemkin.hostname=node$NODE potemkin.net2=10.10.0.1$NODE/24"
  nics=(-device virtio-net-pci,netdev=n0,mac=52:54:00:00:00:1$NODE
        -netdev socket,id=n1,mcast=230.0.0.1:1234 -device virtio-net-pci,netdev=n1,mac=52:54:00:10:00:1$NODE)
else
  nics=(-device virtio-net-pci,netdev=n0)
fi
append+=" $*"
exec env LD_LIBRARY_PATH=$Q/usr/lib/x86_64-linux-gnu "$Q/usr/bin/qemu-system-x86_64" \
  -L "$Q/usr/share/qemu" -L "$Q/usr/share/seabios" -L "$Q/usr/lib/ipxe/qemu" \
  -enable-kvm -cpu host -m ${MEM:-4G} -smp 4 \
  -kernel "$KERNEL" -initrd "build/image-$flavor/initramfs.gz" -append "$append" \
  -drive file=$disk,if=virtio,format=raw \
  -netdev "$uplink" "${nics[@]}" \
  -nographic -no-reboot
