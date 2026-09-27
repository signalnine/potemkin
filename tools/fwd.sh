# One connection of a VM's forwarded API port (QEMU guestfwd cmd:). QEMU runs
# this once per guest connection, so a village that opens thousands of
# connections would fork thousands of relays on the host; 16 slot locks cap it.
# Usage (by run-vm.sh): bash tools/fwd.sh HOST PORT
host=$1 port=$2
dir=${XDG_RUNTIME_DIR:-/tmp}/potemkin-fwd-$(id -u)
mkdir -p "$dir"
for i in $(seq 1 16); do
  exec {fd}>"$dir/slot$i"
  if flock -n "$fd"; then exec nc "$host" "$port"; fi
  exec {fd}>&-
done
exit 1
