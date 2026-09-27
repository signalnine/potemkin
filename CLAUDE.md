# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Status and commands

PoC in progress, tracked in bd (`bd ready`, `bd list --all`; prefix `pk`). `PotemkinOS — Design Doc.md` is the source of truth; `docs/link-audit.md` and `docs/q27-api.md` hold measured facts and the q27 call map.

```sh
bash tools/fetch-toolchain.sh          # tcc + musl -> build/sysroot (apt-get download, no root)
bash tools/fetch-vm.sh                 # QEMU + a checked VM kernel -> build/qemu, build/kernel
bash tools/build.sh test               # /sbin/init + CPU unit tests (host, harness, api), isolated
bash tools/build.sh api                # q27-init without CUDA (llm=api) -> build/q27-init-api
bash tools/build.sh init               # q27-init with q27, PROFILE=12g|w8|full -> build/q27-init-$PROFILE
bash tools/mkimage.sh api|cuda         # initramfs + ext4 disk; INIT=api|12g|w8|full picks the binary
API_URL=... API_MODEL=... API_KEY=... bash tools/run-vm.sh api   # locked uplink; NET_OPEN=1, NODE=N
```

q27 sources come from `Q27` (default `../q27`; on this machine `/mnt/ai/projects/q27-master`, commit 8be624e, with `build/pf4.o`). json.hpp and httplib.h are vendored in `third_party/`. Models: `/mnt/ai/models/bonsai2-27b/q27/*slim.q27` (pair with the 12g build), `/mnt/ai/models/qwen38-27b-mtp/*.q27`, tokenizer `qwen38-27b-mtp.tok`, DFlash2 pack `/mnt/ai/models/qwen38-27b-dflash2-bf16/qwen38-dflash2-q8-serve.d2w`.

Dev runs: `q27-init --root DIR ...` treats DIR as the village; children are chrooted into it via user+mount namespaces (`Config::isolate`), with /proc /sys /dev passed through. `tools/drive.py` drives the console through a pty (`~text` types into a tty-mode child, `~^]^]` sends the escape chord). Pin GPUs with `CUDA_DEVICE_ORDER=PCI_BUS_ID CUDA_VISIBLE_DEVICES=0` (3090) or `=1` (5090); default CUDA ordering puts the 5090 first. The 5090 may be shared with the user's desktop apps; size `--ctx` explicitly there.

Code layout: `src/host` (the eight tools, no CUDA, CONTRACT.md), `src/harness` (agent loop, slash commands, transcript, frozen tools JSON + system prompt), `src/q27` (the only CUDA TU; in-process q27 Engine), `src/api` (OpenAI-compatible SSE backend + fetch), `src/init/init.c` (static PID 1, also `/sbin/rescue`), `src/main.cpp` (q27-init).

## What this is

PotemkinOS: a Linux image with no userland. Boot lands in a chat with a local model (via [q27](https://github.com/signalnine/q27)), and the model writes every userland program itself, in C, compiled by tcc, at runtime. It is a joke with a working build. The design doc's Non-goals section is deliberate: not pure, not secure (children run as root in v1), not a reconciler. Don't argue those back in, and don't pull Stretch items (PTX backend, capability manifests/seccomp/Landlock, reconcile-on-boot, Metal, the agent board) into v1.

Priority order for every decision: funny > working > safe. The failure mode to avoid is competence: the harness protects the inference process and the user's ability to undo, and must not quietly stop the model from doing something dumb (no guardrails, validation, or "helpful" fixups on what the model writes or runs).

## Architecture (the parts that span the doc)

**Process tree.** `/sbin/init` is a ~50-line C supervisor (PID 1: reap, mount `/data /state /generated`, respawn). `q27-init` is a fork of `q27-server` sharing its `Session`; it owns the console on `/dev/tty1` and runs the tool loop. The inference process is never PID 1. The console path never loops back through HTTP (HTTP stays in the build only for remote debugging). `/sbin/rescue` exists via `init=/sbin/rescue` but is not on the model's `PATH`.

**Eight frozen host primitives**: `read`, `write`, `stat` (`list=1` walks), `spawn`, `wait` (`signal=` sends first), `compile`, `snapshot` (`rollback=`), `fetch` (netboot only). The JSON block is `kToolsJson` in `src/harness/harness.cpp`. The system+tools block is the prefix-cache cut, so any change to the tool set invalidates every cached conversation. Hard rules that follow from this:
- No `bash`/shell tool, no dynamic tool registration, no MCP, no "model adds a tool". New capability = model writes a binary and `spawn`s it.
- No `ps` tool; the model reads `/proc` or writes its own.
- Skills are never put in the system prompt (would move the cache cut).

**Tool-call dialect.** Tools render through q27's existing qwen35 XML template path, `<tool_call>`/`<tool_response>` as vocab added tokens. No JSON function-calling shim. Arguments are flat strings/ints only (`spawn`'s `argv` is one string split by the harness). Parse tool calls from the completed turn, not the stream. Multiple calls per turn run sequentially, one response each, in order. Staying in this dialect is what keeps q27's 22-mode drift corpus protecting the parser; new drift shapes go into that corpus. `read` returns text or a hexdump page, never base64. Tool results truncate at ~4K tokens with a truncation marker. Compiler errors are normal tool responses.

**Sampler** is fixed: `--think --temp 1.0 --top-p 0.95 --top-k 20 --min-p 0.05 --think-budget 0`, `--constrain-tools` off. Not greedy (measured 0.511 vs 0.928).

**spawn modes**: `capture` (buffered, default timeout 60 s), `background` (logs to `/state/log/<pid>`), `tty` (child owns `/dev/tty1`; escape chord `Ctrl-]` twice kills its process group). Every child gets a cgroup v2 `potemkin/<pid>` with `memory.max` (default 512 MB) and `pids.max` (default 256); `q27-init` sets its own `oom_score_adj` to -1000. Harness writes `/state/procs` after every change. `rollback(id)` also kills every child spawned after that snapshot.

**compile + store.** `compile` is content-addressed, not `spawn(tcc)`: `/store/sha256:<hash>/{source.c,bin,manifest.json}`, with `/generated/bin/<name>` symlinked in. Only `c` in v1. The store is append-only and is the only source tree / reproducibility ledger. Snapshots are copy-based (`/snapshots/<id>`), the prefix cache lives in `/cache` (not snapshotted), `/intent/log` is the intent file. See the design doc's Implementation notes for other deviations.

**Filesystem.** Persistent: `/data` (not snapshotted), `/state` and `/generated` (snapshotted per turn via btrfs subvolumes, overlayfs on VM), `/store` (append-only), `/intent` (append-only log, never enforced). Everything else is read-only initramfs. Snapshot happens before any turn that calls `write`, `spawn`, or `compile`.

**Console.** Lines starting with `/` go to the harness, never the model. Fixed set: `/help /undo /procs /log /kill /skill /reboot`. No `/model`, `/settings`, `/clear`. A skill is `/generated/skills/<name>.md`, first line = description.

**Backends.** One image; kernel cmdline selects `llm=api|cuda|cpu` and `model=qwen|bonsai`. Qwen3.8-27B-MTP + DFlash2 drafter on 24 GB+ cards; Bonsai 2 27B (ternary, ~6 GB) below that, on CPU, and on Orin. `ip=dhcp` provides networking with no userland; `api_key=` authenticates netboot.

## Trusted base

Only kernel/initramfs, NVIDIA stack, libc + CUDA static libs, `q27-init`, weights + tokenizer, tcc + musl headers, rescue init, and certs (netboot only). Never add BusyBox, coreutils, a shell, `make`, `git`, `curl`, `python`, `systemd`, or a package DB to the image. The harness must not be Claude Code or any Node-based/shell-assuming agent.
