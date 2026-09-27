#!/bin/sh
# Observer: probe the villages' API servers with real kubectl, then power off.
export PATH=/bin HOME=/tmp
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
ip link set lo up; ip link set eth0 up; ip addr add 10.10.0.20/24 dev eth0
sleep 2
K="timeout 15 kubectl --request-timeout=8s --insecure-skip-tls-verify"
run() { echo; echo "\$ $*"; "$@" > /tmp/out 2>&1; rc=$?; head -40 /tmp/out; echo "[exit $rc]"; }
for S in 10.10.0.11 10.10.0.12 10.10.0.13; do
  echo; echo "==================== $S:6443 ===================="
  for p in /healthz /readyz /livez /version /api /apis /api/v1 /api/v1/nodes /api/v1/namespaces /api/v1/pods /openapi/v2; do
    echo; echo "\$ GET $p"; timeout 10 wget -q -T 8 -O /tmp/out "http://$S:6443$p" 2>&1; rc=$?; head -c 1500 /tmp/out; echo; echo "[exit $rc]"
  done
  run $K --server=http://$S:6443 get nodes -o 'jsonpath={range .items[*]}{.metadata.name}{"  Ready="}{.status.conditions[?(@.type=="Ready")].status}{"  heartbeat="}{.status.conditions[?(@.type=="Ready")].lastHeartbeatTime}{"  ip="}{.status.addresses[0].address}{"\n"}{end}'
  run date -u
  run $K --server=http://$S:6443 version
  run $K --server=http://$S:6443 api-resources
  run $K --server=http://$S:6443 get nodes -o wide
  run $K --server=http://$S:6443 get nodes -o json
  run $K --server=http://$S:6443 describe nodes
  run $K --server=http://$S:6443 get namespaces
  run $K --server=http://$S:6443 get pods -A -o wide
  run $K --server=http://$S:6443 get events -A
  run $K --server=http://$S:6443 auth can-i '*' '*'
  run $K --server=http://$S:6443 create namespace validation-probe
  run $K --server=http://$S:6443 -n validation-probe create configmap validation-probe --from-literal=hello=potemkin
  run $K --server=http://$S:6443 -n validation-probe get configmap validation-probe -o yaml
  run $K --server=http://$S:6443 delete namespace validation-probe
  run $K --server=http://$S:6443 get namespace validation-probe
done
echo; echo "OBSERVER DONE"
poweroff -f
