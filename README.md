# PotemkinOS

A Linux image with no userland. You boot into a chat session with a local
model, and every program you can invoke was written by that model, at boot,
on this machine, and it shows.

There is no `/bin`, no shell, no package manager, no coreutils. There is a
kernel, an inference engine ([q27](https://github.com/signalnine/q27)), a C
compiler, and a prompt. Ask for what you want and the model writes a facade of
a userland in front of you. Every install is a different village.

![A fresh village: the model writes ls, then a shell, then hands over the console](docs/demo.gif)

Qwen3.8-27B on an RTX 5090, first boot of an empty village: 17 minutes of wall
time, played at 5x with thinking at 60x. Asked for a shell, it also wrote
`ptytest`, `procscan` and `fdscan` to debug its own shell before handing over
the console.

```text
[ potemkin/1 ]

hello. there is nothing here yet.

> what's on this disk
  compile ls.c
    exit=0 sha256:3c9a77f0...
  spawn /generated/bin/ls /
```

## Status

It boots. In a VM the `llm=api` image comes up with DHCP, talks to any
OpenAI-compatible endpoint, and the model writes, compiles and runs C inside
the VM as root. State survives a hard kill. On a workstation `q27-init` runs
the same loop against local GPUs:

| Where | Model | tok/s |
| --- | --- | --: |
| RTX 5090 | Qwen3.8-27B default tier + DFlash2 Q8, 262K window | 312-365 |
| RTX 3090 | Qwen3.8-27B q4s + DFlash2 Q8, W_MAX=8 build | 162-169 |
| RTX 3090 | Bonsai 2 27B T3 + MTP heads, 12g build | 104-148 |
| RTX 3090 | Bonsai 2 27B T3, 12g build | 55-79 |

Asked for a shell, Qwen plans for six minutes, writes `sh` plus the `ls`,
`cat` and `rm` it needs, fixes the shell three times and hands you the
console. Its `ls` prints every symlink with mode `0777`, and the shell exits
after the first command. Unbounded, Bonsai plans for sixteen minutes and
writes nothing; with an 8K think budget (its default) it writes a working shell
in under two minutes, then starts it a second time right as you type your
next question to it, which the shell reports as `how: not found`. All of this
is working as intended.

Not done: booting the cuda image on bare metal (the procedure is below and
untested), `llm=cpu`, and the Orin.

## What ships

Only what the model needs before it can speak. Measured for the 12 GB path
([docs/link-audit.md](docs/link-audit.md)):

| Component | Why | Size |
| --- | --- | --: |
| Linux kernel | Not writing a kernel | 17 MB |
| `nvidia.ko`, `nvidia-uvm.ko`, GSP firmware, libcuda | Hardware | 190 MB |
| glibc, 5 libraries | q27, tcc and libcuda load it | 6 MB |
| `/sbin/init`, also `/sbin/rescue` | PID 1: mounts, driver, DHCP, keeps the model alive | 0.9 MB |
| `/usr/bin/q27-init` | Engine, tool loop, console | 18 MB |
| tcc + musl | The model's compiler, static output | 4 MB |
| Weights + tokenizer | The distribution *is* the weights | 6-23 GB |
| CA certificates | `llm=api` only | 0.2 MB |

BusyBox, coreutils, shells, make, git, curl, python and systemd stay out. The
`llm=api` initramfs is 7.8 MB.

## The eight tools

`read`, `write`, `stat` (`list=1` walks a directory), `spawn` (`capture`,
`background` or `tty`), `wait` (`signal=` sends first), `compile` (C through
tcc, stored by content hash in `/store`), `snapshot` (`rollback=` restores),
and `fetch` (netboot only). The model gets no shell. If it wants `ps`, it
writes `ps`.

The harness owns every line that starts with `/`: `/help /undo /procs /log
<pid> /kill <pid> /skill <name> [args] /reboot`. Ctrl-C stops generation, and
Ctrl-] twice takes the console back from whatever the model ran in tty mode.

## Building

You need a q27 checkout (default `/mnt/ai/projects/q27-master`, with its
`build/pf4.o`), CUDA 12.8+ and gcc. tcc, musl, qemu and the VM kernel come
from the distro archive through `apt-get download`: no root, no package
scripts.

```sh
bash tools/fetch-toolchain.sh          # tcc + musl -> build/sysroot
bash tools/build.sh test               # init + unit tests, CPU only
bash tools/build.sh init               # q27-init, 12g shape (sm_86, slim packs)
PROFILE=w8 bash tools/build.sh init    # tri-arch, 24 GB cards
PROFILE=full bash tools/build.sh init  # tri-arch, 32 GB cards
bash tools/mkimage.sh api              # initramfs + persistent disk
NET=dhcp bash tools/run-vm.sh api      # boot it in qemu on the serial console
```

One test: `./build/test_host <name-substring>`, same for `test_harness` and
`test_api`. The test binaries spawn, signal and read devices, so
`tools/build.sh` runs each one in its own session with a memory cap.

### Without rebooting anything

`q27-init --root DIR` runs the whole thing against a directory. The model's
programs get chrooted into it through a user namespace, so they see the
village as `/` the same way they would on the image.

```sh
./build/q27-init-sm86 --model bonsai2-27b-t3-mtp-slim.q27 --tok qwen38-27b-mtp.tok \
  --root /tmp/village --sysroot $PWD/build/sysroot --ctx 131072 --engine-log /tmp/engine.log
```

`tools/drive.py` types into it through a pty for scripted runs; `--cast FILE`
records an asciicast, and `tools/cast2gif.py FILE out.gif` renders it (Pillow
and ffmpeg).

### On real hardware (llm=cuda)

The cuda initramfs carries NVIDIA modules built for the running kernel, so
boot it with that kernel.

1. `bash tools/build.sh init` with the profile for your card, then
   `ARCH=86|w8|full bash tools/mkimage.sh cuda`.
2. Format a spare partition ext4 or btrfs and put the weights in `/models` on
   it: `qwen.q27` or `bonsai.q27`, `qwen38.tok`, and optionally
   `qwen-dflash2.d2w`. DFlash2 turns on when that file exists. Init creates
   the other trees on first boot.
3. Add a boot entry with `/boot/vmlinuz-$(uname -r)`,
   `build/image-cuda/initramfs.gz`, and a command line like
   `potemkin.disk=/dev/nvme1n1p2 llm=cuda model=qwen ip=dhcp`. Add
   `init=/sbin/rescue` for the rescue menu.

Expect thirty to sixty seconds of black screen on `/dev/tty1` while the
weights load.

## Backends

The kernel command line picks: `llm=cuda|api` and `model=qwen|bonsai`.
`llm=api` takes `api_url=`, `api_model=` and `api_key=`, and yes, the API key
goes on the kernel command line. For development, `PK_API_KEY` in the
environment works too.

## Oblasts

An oblast is a cluster of Potemkin villages. `NODE=N bash tools/run-vm.sh api`
boots village N with its own disk and hostname, plus a second NIC on a private
LAN (10.10.0.1N) shared with the other villages over QEMU multicast. The
uplink reaches only the model API, because an unattended village with an open
uplink will scan your host. `tools/lansniff.py` watches the LAN from the host,
`tools/mkobserver.sh` builds a VM that probes the villages' API servers with
the official `kubectl`, and `tools/oblast2gif.py` renders the recordings side
by side.

[Oblast 1](docs/oblast-1.md): three villages told to form a Kubernetes
cluster. After 8.5 hours, two hints and 190 programs, the real `kubectl`
listed all three nodes Ready through an API server one of them wrote in C.

![Oblast 1](docs/oblast-1.gif)

## Orin

L4T downstream kernel, device tree, boot firmware blobs, Tegra libcuda. More
NVIDIA, less Linus. sm_87 is not in the tri-arch build yet.

## Someday

A PTX backend for `compile` so the model can rewrite its own inference engine;
capability manifests on `spawn`; a reconcile-on-boot flag so someone can film
a village regenerating from `/intent`; an agent-only board where villages
trade skills. (what could go wrong??)
