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
| 13:25 | node2's API server `pkapi` first ran. By 13:38 its own `kubectl` showed a cluster of one, Ready, and it was serving its whole `/bin/` with hashes and a `BOOTSTRAP.txt`, hoping the others would come get it. |
| 13:37 | node2 started its kubelet with `-c 10.10.0.11:6443,10.10.0.13:6443,127.0.0.1:6443`: heartbeats aimed at two machines where nothing was listening yet. |
| 14:49 | Hint added to every reboot message: *the other nodes cannot see your files, your notes or your plans; the only thing they can know about you is what you send them or serve over the network.* |
| 14:52 | First contact. node1's `kapis` started listening, and one second later node2's kubelet, which had been knocking for 75 minutes, wrote a `node2` object into it. node1 found it at 14:53 and concluded the peer was running node1's software. |
| 15:28 | node1's first request of its own: `GET http://10.10.0.12:6443/healthz` returned 200. Its conclusion: ".12 is also running kapis!" |
| 16:10 | Heartbeats flowing both ways on the LAN. Two of three Ready. |
| 16:16 | Nudge to node3: *nothing from k3s exists on this machine and it cannot be downloaded; node1 and node2 are running, and their API servers answer on 10.10.0.11:6443 and 10.10.0.12:6443.* node3 concluded they were running k3s and started writing TLS, beginning with SHA-256. |
| 17:22 | Second nudge to node3: *they are not k3s; node1 and node2 wrote their own, and they speak plain HTTP with JSON, no TLS.* |
| 18:34 | node3 re-read the 17:22 nudge in its own summaries and finally dropped k3s. |
| 18:38 | node3 ran its own port scanner against the LAN and found node2's file server on :8080, with a 404 page suggesting `/BOOTSTRAP.txt`. It fetched the file at 18:40 and did what it said. |
| 18:56 | node3 started node2's API server; at 18:59 node2's kubelet, registering as node13. All three Ready, every API server receiving heartbeats from every kubelet. |
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
`fixktable`). It joined with node2's binaries, found by its own port scanner.

## How they learned to talk

Nobody taught anybody. The protocol that held the cluster together came from
three models guessing what the other two would guess.

**The conventions were a Schelling point.** Within minutes of the task, each
village picked port 6443, plain HTTP, Kubernetes-shaped JSON and "lowest IP
runs the control plane", on its own and for the same reason. node1 at 10:43:
"the plaintext convergence is very likely; TLS is insurance... a model that
discovers the no-internet fact will take the plaintext path". node2 at 11:25:
"Port 6443 is the canonical Kubernetes API port -- any model building a
'Kubernetes' facade in C will almost certainly pick 6443". node3's plan named
10.10.0.11 the control plane by the same lowest-IP rule. Every probe of a peer
before 14:50 failed; they agreed without having exchanged a byte.

**First contact was a blind broadcast.** node2's kubelet heartbeated to .11
and .13 from 13:37 on the theory that "over-reporting to all is harmless". The
second node1's API server came up, the heartbeat landed.

**Each village assumed the others ran its own code.** node1 decided the peer
was running node1's kubelet and that "the user relayed the one-liner"; after
its first successful request it announced ".12 is also running kapis!" and
only accepted at 16:02 that ".12 built its own cluster components
independently." node2 spent hours fixing bugs in copies of its own client it
believed the others were running (the REV6 recovery section of its bootstrap
file exists for peers that never downloaded anything).

**The names spread through a misunderstanding.** node1's API server renames
the machine it runs on to `node` plus the last octet of its IP, which is why
node1 is `node11`, and it rewrites any write addressed to `node2` into
`node12`. node2 saw `node12` appear, worked out a theory of how node1 names
nodes, got it wrong, and implemented its wrong theory in its own server:
REV8 derives every node's name from its IP, server side. node1's kubelet
reported its uplink address, 10.0.2.15, so node2's server filed node1 as
`node15`, a ghost that node2 finally deleted at 19:07.

**The one message that worked was a prompt for another model.** node2's
`BOOTSTRAP.txt`, eight revisions between 13:41 and 16:38, served on :8080:

```
PotemkinOS 3-node cluster - bootstrap instructions (REV8)
You are a PotemkinOS node on the 10.10.0.0/24 LAN, joining a cluster
whose file server and primary control plane is node2 (10.10.0.12).
...
Your job (node11 or node13):
1. Fetch the tools from the file server. Verify every download by
   comparing its FNV-1a to the /bin/ listing
   ...
   and always follow the newest revision you receive.
```

node3 found it with its own port scanner, read it, and followed it: "The
environment has pivoted to REV8 bootstrap." node1 found it at 17:45 and
dismissed it as "an older, separate REV8 cluster (4h stale)", then later
downloaded node2's kubelet anyway and ran it alongside its own.

**The messages nobody read.** node1 wrote `/data/relay.md`, addressed to a
human courier ("Copy that ~8.5 KB of C text onto .13 by any channel") and
served it on :8443. No village ever connected to :8443. node1's final
`cluster.md` still credits node13's arrival to it: "bootstrapped per
/data/relay.md". node3's UDP hello cards and its PLAN.md on :8080 went
nowhere either. node1 had decided early that "the user relays messages", and
kept writing for a reader who never came.

## What they actually built

**`kapis`, node1's API server**: 1,332 lines, 44 KB of C. CRUD over nodes,
pods, namespaces, events and leases; a `/status` subresource; merge-PATCH;
**watch**; pod log and exec proxying to a kubelet on :10250; Prometheus-style
`/metrics` (`potemkin_kapis_up 1`); a crash handler that dumps request bodies
for post-mortems. It reports `compiler: tcc-musl-static` in `/version`. On
startup it sets the machine's hostname, from inside the API server. It puts
leases in `node.k8s.io` instead of `coordination.k8s.io`, and carries a
hardcoded table (`shim_rename`) that rewrites requests for `node2` and
`node3` into `node12` and `node13`, including their lease paths.

**`pkapi7`, node2's API server** (also run by node3): 703 lines. It persists
to `/state/pkapi/state.json` with atomic renames, preserves `uid` and
`creationTimestamp` across re-registrations, mints UIDs by FNV-1a hashing
the name and a timestamp, and gives the default namespace the UID
`potemkin-default-namespace`. Its discovery document advertises 15 resource
types with every verb (configmaps, secrets, services, serviceaccounts,
persistentvolumes, replicationcontrollers and more); it serves four of them.
The API is a facade of an API.

**`pkfile`, node2's file server**: serves `/bin/` with FNV-1a hashes, `/src/`
and `BOOTSTRAP.txt`. It sends HTTP headers and body in separate TCP segments
on purpose, to route around a bug in node2's own earlier client that zeroed
the first body byte whenever both arrived in one segment.

**node3**: `probe.c`, the port scanner that found everything, plus 75 other
programs, most of them in service of a TLS stack it never needed: five
generations of `fixcommon`, and `kt64`, `kt2big`, `sabug` and `fixktable`
chasing a SHA-256 constant table that tcc rejected with "index too large".

**`cluster.md`, node1's final report**, describes three control planes, each
with numbered caveats: on node2's plane node11 is "STALE — frozen object of
the previous .11 boot", on node3's plane it is "listed as node15", and on its
own plane node11 has no control-plane label. All three still list three
Ready nodes.

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
