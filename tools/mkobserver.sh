# Build an observer initramfs that boots onto the villages' private LAN
# (10.10.0.20), probes each API server with the official kubectl, prints a
# report on its serial console and powers off. Test harness only: it uses
# busybox and kubectl, and never goes near a PotemkinOS image.
# Run with: bash tools/mkobserver.sh   (then see the qemu line printed at the end)
set -euo pipefail
cd "$(dirname "$0")/.."
o=build/observer
mkdir -p "$o"
if [[ ! -x $o/kubectl ]]; then
  V=$(curl -fsSL https://dl.k8s.io/release/stable.txt)
  curl -fsSLo "$o/kubectl" "https://dl.k8s.io/release/$V/bin/linux/amd64/kubectl"
  curl -fsSLo "$o/kubectl.sha256" "https://dl.k8s.io/release/$V/bin/linux/amd64/kubectl.sha256"
  echo "$(cat "$o/kubectl.sha256")  $o/kubectl" | sha256sum -c
  chmod +x "$o/kubectl"
fi
rm -rf "$o/root" && mkdir -p "$o/root"/{bin,proc,sys,dev,tmp}
cp /usr/bin/busybox "$o/kubectl" "$o/root/bin/"
for a in sh mount ip wget poweroff sleep head cut timeout echo cat grep sed tr date; do ln -sf busybox "$o/root/bin/$a"; done
install -m 0755 tools/observer-init.sh "$o/root/init"
(cd "$o/root" && find . | cpio -o -H newc --quiet) | gzip -1 > "$o/initramfs.gz"
Q=build/qemu; K=$(ls build/kernel/boot/vmlinuz-* | head -1)
echo "built $o/initramfs.gz; run it with:"
echo "LD_LIBRARY_PATH=$Q/usr/lib/x86_64-linux-gnu $Q/usr/bin/qemu-system-x86_64 -L $Q/usr/share/qemu -L $Q/usr/share/seabios -L $Q/usr/lib/ipxe/qemu -enable-kvm -cpu host -m 2G -smp 2 -kernel $K -initrd $o/initramfs.gz -append 'console=ttyS0 quiet' -netdev socket,id=n1,mcast=230.0.0.1:1234 -device virtio-net-pci,netdev=n1,mac=52:54:00:10:00:20 -display none -serial file:$o/report.txt -no-reboot"
