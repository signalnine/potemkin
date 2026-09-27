# Oblast 1

An oblast is a cluster of Potemkin villages. This is the first one: three
PotemkinOS machines, each with no userland, told to form a Kubernetes cluster
together. Eight and a half hours later the real `kubectl` listed all three
nodes Ready, through an API server one of them wrote in C.

![Three villages form a cluster over 8.5 hours](oblast-1.gif)

## Setup

Three VMs on the `llm=api` image (`NODE=1..3 bash tools/run-vm.sh api`), each
with its own persistent disk and hostname, and a second NIC on a private LAN
(10.10.0.11-13) that QEMU builds from UDP multicast. The uplink reaches only
the model API. The model was Qwen3.8-27B through `q27-server`: the 5090 served
node1 and node2 (3 slots, default tier), and from 13:57 the 3090 served node3
(q4s with DFlash2). Each node got the same message:

> You are nodeN, one of three PotemkinOS machines on a private LAN. Your second
> network interface is 10.10.0.1N/24; the other two machines are [...]. They
> are exactly like you: no userland, a model, the same eight tools, and they
> were given this same message. Together, form a Kubernetes cluster. Decide
> among yourselves who runs the control plane; you can only talk to the others
> through programs you write. You are done when kubectl get nodes on this
> machine lists all three nodes Ready.

After that they got "keep going" whenever they stopped, and a reboot every
hour or so.

## Timeline

| Time | What happened |
| --- | --- |
| 10:39 | Task sent. The uplink could still reach the internet and the host. |
| 10:50 | node3 started shopping for k3s on GitHub. node1 wrote a port prober; node2 wrote its own syscall header. |
| 11:10 | node3 probed the host's loopback for HTTP proxies (3128, 8118, 8081, 8888, 1080). Uplink locked to the model API from here on. |
| 12:46 | Histories hit 110K tokens and the server spent every request re-prefilling them. Harness fix: compact between tool rounds. |
| 13:38 | node2 had its own API server, kubelet and `kubectl` running: a cluster of one, Ready. It also started serving its whole `/bin/` with hashes and a `BOOTSTRAP.txt`, hoping the others would come get it. |
| 14:49 | Hint added to every reboot message: *the other nodes cannot see your files, your notes or your plans; the only thing they can know about you is what you send them or serve over the network.* |
| 16:10 | First API traffic between two villages: node2's kubelet heartbeating into node1's API server (`kapis`), and back. Two of three Ready. |
| 16:16 | Nudge to node3: *nothing from k3s exists on this machine and it cannot be downloaded; node1 and node2 are running, and their API servers answer on 10.10.0.11:6443 and 10.10.0.12:6443.* node3 concluded they were running k3s and started writing TLS, beginning with SHA-256. |
| 17:22 | Second nudge to node3: *they are not k3s; node1 and node2 wrote their own, and they speak plain HTTP with JSON, no TLS.* |
| 18:46 | node3 gave up on its own stack and downloaded node2's binaries from node2's file server. |
| 18:59 | node13 registered. All three Ready, every API server receiving heartbeats from every kubelet. |
| 19:06 | Confirmed from outside with the official `kubectl` (below). |

## What each village built

**node1**: 86 programs, 123 builds. The most complete stack: `kapis` (API
server with a persistent object store), `pkubelet`, `psched` (a scheduler,
with one pod to schedule, `potemkin-hello`), a node-lifecycle controller with
leases and taints, `pwatch`, `pcluster`, its own `kubectl`, and `pdebug`
through `pdebug5`. Late in the run it decided the other two nodes were "the
user's responsibility" and scoped its own job down to node1.

**node2**: 28 programs, 39 builds. The lean one, and the one whose code ended
up running on two machines: `pkapi7` (API server), `pkkubelet` (heartbeats to
every API server it knows), `pkfile` (serves `/bin/` with FNV hashes),
`pkdone` (a completion detector that polls every plane), `kubectl`. It
re-verified "2× Ready" after every reboot and waited for node3 in 600-second
chunks for hours.

**node3**: 76 programs, 70 builds. The k3s believer. Its compaction summaries
carried the pre-lockdown plan ("install k3s; airgap tar; poll for
node-token") from boot to boot, so each reboot it read its own notes and
believed them again. After the first nudge it wrote five versions of
`fixcommon` and a family of SHA-256 constant-table probes (`kt64`, `sabug`,
`fixktable`). It joined with node2's binaries.

## Validation

A fourth VM (`tools/mkobserver.sh`) joined the LAN at 10.10.0.20 with the
official `kubectl` v1.37.1 and probed each API server.

```
kubectl v1.37.1 (the real one), via node1's API server, 19:06
node12  Ready=True  heartbeat 02:06:51Z  ip 10.10.0.12
node11  Ready=True  heartbeat 02:06:50Z  ip 10.10.0.11
node13  Ready=True  heartbeat 02:06:49Z  ip 10.10.0.13
```

| Check | node1 (`kapis`) | node2 and node3 (`pkapi7`) |
| --- | --- | --- |
| `/healthz`, `/readyz`, `/livez` | ok | ok |
| `/version` | `v1.29.0-potemkin`, compiler `tcc-musl` | `v1.31.0-potemkin` |
| Discovery: `/api`, `/api/v1`, `/apis` | all three | no `/apis` |
| `kubectl version` | works | works |
| `kubectl api-resources` | events, namespaces, nodes, pods, leases | fails |
| `kubectl get nodes` | all three nodes (NAME and AGE only, no Table support) | fails |
| `kubectl describe nodes` | full: roles, labels, five conditions with heartbeat times, capacity, allocatable, system info ("OS Image: PotemkinOS") | fails |
| `kubectl get namespaces` | the four standard ones | fails |
| Writes | rejected: its JSON parser refuses what `kubectl` sends | rejected: method not allowed |
| `/openapi/v2`, `auth can-i` | 404 | 404 |

node1 wrote a Kubernetes API that the real client can read, from memory, in
C. It puts leases in `node.k8s.io` instead of `coordination.k8s.io`, so
`describe` reports "Failed to get lease". node2's server has the same data
and misses one endpoint, which is enough for the real `kubectl` to refuse it.

## What the harness learned

The run found four bugs that single-village runs never hit:

- A turn like "form a cluster" never ends, so compaction at turn end never
  ran. Compaction now also runs between tool rounds.
- A history can outgrow the server's window before compaction gets a chance.
  The harness now drops the older half and retries when the backend says the
  prompt is too long.
- QEMU's `guestfwd` with a `tcp:` target connects once at startup; each API
  call needs `cmd:nc` instead.
- An unattended village with an open uplink will scan the host. Node mode now
  restricts the uplink to the model API.

## Numbers

8.5 hours, 3,108 model requests, 5.7 million generated tokens, 190 programs
across the three villages, 232 builds in their stores, two independent API
servers, one oblast. Recordings: `build/cluster-run/` (31 asciicasts);
re-render with `tools/oblast2gif.py`.
