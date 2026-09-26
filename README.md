# PotemkinOS

A Linux image with no userland. You boot into a chat session with a local
model, and everything you can type `ls` at was written by that model, at boot,
on this machine, and it shows.

There is no `/bin`, no shell, no package manager, no coreutils. There is a
kernel, an inference engine ([q27](https://github.com/signalnine/q27)), a C
compiler, and a prompt. Ask for what you want and the model writes a facade of
a userland in front of you. Every install is a different village.

```text
[ potemkin/1 ]

hello. there is nothing here yet.

> what's on this disk
  compile ls.c
    exit=0 sha256:3c9a77f0...
  spawn /generated/bin/ls /
```

The design, and why every decision went the way it did, is in
[PotemkinOS -- Design Doc.md](PotemkinOS%20%E2%80%94%20Design%20Doc.md). The short
version: funny first, working second, safe third.

## What ships

Only what the model cannot write for itself because it needs it to exist
before it can speak. Measured for the Bonsai / 12 GB path
([docs/link-audit.md](docs/link-audit.md)):

| Component | Why | Size |
| --- | --- | --: |
| Linux kernel | Not writing a kernel | 17 MB |
| NVIDIA `nvidia.ko` + `nvidia-uvm.ko`, GSP firmware, libcuda | Hardware enablement | 190 MB |
| glibc (5 libraries) | What q27, tcc and libcuda load | 6 MB |
| `/sbin/init` (also `/sbin/rescue`) | PID 1: mounts, driver, network, keeps the model alive | 0.9 MB |
| `/usr/bin/q27-init` | The engine, the tool loop, the console | 11-18 MB |
| tcc + musl | The model's compiler; static output | 4 MB |
| Weights + tokenizer | The distribution *is* the weights | 6-23 GB |
| CA certificates | `llm=api` / netboot only | 0.2 MB |

Not shipped: BusyBox, coreutils, any shell, make, git, curl, python, systemd,
a package database, `/bin`. The `llm=api` initramfs is 7.8 MB.

## The eight tools

The model gets `read`, `write`, `stat` (`list=1` walks a directory), `spawn`
(`capture`, `background`, `tty`), `wait` (with `signal`), `compile` (C via
tcc, content-addressed into `/store`), `snapshot` (with `rollback`), and
`fetch` (netboot only). No shell. If it wants `ps`, it writes `ps`.

Console commands, handled by the harness and never seen by the model:
`/help /undo /procs /log <pid> /kill <pid> /skill <name> [args] /reboot`.
Ctrl-C stops generation; Ctrl-] twice takes the console back from a program
the model ran in tty mode.

## Building

Needs a q27 checkout (default `/mnt/ai/projects/q27-master`, with
`build/pf4.o` built), CUDA 12.8+, gcc, and the distro archive for tcc, musl,
qemu and a kernel (fetched with `apt-get download`, no root, no package
scripts run).

```sh
bash tools/fetch-toolchain.sh          # tcc + musl -> build/sysroot
bash tools/build.sh test               # init + unit tests (CPU only)
bash tools/build.sh init               # q27-init, 12g shape (sm_86, slim packs)
PROFILE=full bash tools/build.sh init  # q27-init, tri-arch (sm_86/89/120)
bash tools/mkimage.sh api              # initramfs + persistent disk
bash tools/run-vm.sh api               # boot it in qemu (serial console); NET=dhcp to lease
```

Run a single test: `./build/test_host <name-substring>` (same for
`test_harness`, `test_api`).

### Running without rebooting anything

`q27-init --root DIR` runs the whole thing against a directory: the model's
programs are chrooted into it through a user namespace, so they see the
village as `/` just as they would on the real image.

```sh
./build/q27-init-sm86 --model bonsai2-27b-t3-slim.q27 --tok qwen38-27b-mtp.tok \
  --root /tmp/village --sysroot $PWD/build/sysroot --ctx 131072 --engine-log /tmp/engine.log
```

`tools/drive.py` drives it from a script through a pty.

### Booting it on real hardware (llm=cuda)

The cuda initramfs carries `nvidia.ko` and `nvidia-uvm.ko` built for the
running kernel, so boot it with that same kernel.

1. Build: `PROFILE=full bash tools/build.sh init` (5090/4090),
   `PROFILE=w8` (24 GB cards) or the default 12g build (12 GB and under), then
   `ARCH=full|w8|86 bash tools/mkimage.sh cuda`.
2. A spare partition, ext4 or btrfs, becomes the persistent disk. Put the
   weights in `/models` on it: `qwen.q27` or `bonsai.q27`, `qwen38.tok`, and
   optionally `qwen-dflash2.d2w` (DFlash2 turns on when it is present). The
   other trees (`data state generated store intent cache snapshots`) are
   created on first boot.
3. Add a boot entry using `/boot/vmlinuz-$(uname -r)`, the initramfs from
   `build/image-cuda/initramfs.gz`, and a command line like
   `potemkin.disk=/dev/nvme1n1p2 llm=cuda model=qwen ip=dhcp`. Add
   `init=/sbin/rescue` for the rescue menu.

The console is `/dev/tty1` on the firmware framebuffer. Thirty to sixty
seconds of black screen while the weights load, then the model.

## Backends

One image; the kernel cmdline picks: `llm=cuda|api` and `model=qwen|bonsai`.

| Target | Model | Measured |
| --- | --- | --- |
| RTX 5090 | Qwen3.8-27B q4s + DFlash2 Q8 | 125-221 tok/s (sharing the card with a running game) |
| RTX 3090 (as a 3060 stand-in) | Bonsai 2 27B T3 slim, 12g build | 55-79 tok/s |
| VM, `llm=api` | any OpenAI-compatible endpoint (OpenAI, vLLM, llama.cpp, OpenRouter, q27-server) | |
| VM, `llm=cpu` | not built yet | |
| Orin Nano Super | not built yet | |

`api_key=` goes on the kernel command line, which is very funny and also
correct. For development, `PK_API_KEY` in the environment works too.

## Orin caveats

L4T downstream kernel, device tree, boot firmware blobs, Tegra libcuda. More
NVIDIA, less Linus. sm_87 is not in the tri-arch build yet.

## Someday

A PTX backend for `compile` so the model can rewrite its own inference engine;
capability manifests on `spawn`; a reconcile-on-boot flag so someone can film
a village regenerating from `/intent`; an agent-only board where villages
trade skills. None of it is in v1.
