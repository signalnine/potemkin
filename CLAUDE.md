# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Status

Pre-code. The only file is `PotemkinOS — Design Doc.md`, which is the source of truth. No build, lint, or test commands exist yet. Not a git repo yet. Update this file when the first code lands (PoC step 1 is a static-link audit of q27; step 2 is `q27-init`).

## What this is

PotemkinOS: a Linux image with no userland. Boot lands in a chat with a local model (via [q27](https://github.com/signalnine/q27)), and the model writes every userland program itself, in C, compiled by tcc, at runtime. It is a joke with a working build. The design doc's Non-goals section is deliberate: not pure, not secure (children run as root in v1), not a reconciler. Don't argue those back in, and don't pull Stretch items (PTX backend, capability manifests/seccomp/Landlock, reconcile-on-boot, Metal, the agent board) into v1.

Priority order for every decision: funny > working > safe. The failure mode to avoid is competence: the harness protects the inference process and the user's ability to undo, and must not quietly stop the model from doing something dumb (no guardrails, validation, or "helpful" fixups on what the model writes or runs).

## Architecture (the parts that span the doc)

**Process tree.** `/sbin/init` is a ~50-line C supervisor (PID 1: reap, mount `/data /state /generated`, respawn). `q27-init` is a fork of `q27-server` sharing its `Session`; it owns the console on `/dev/tty1` and runs the tool loop. The inference process is never PID 1. The console path never loops back through HTTP (HTTP stays in the build only for remote debugging). `/sbin/rescue` exists via `init=/sbin/rescue` but is not on the model's `PATH`.

**Eight frozen host primitives**: `read`, `write`, `stat`/`walk`, `spawn`, `wait`/`signal`, `compile`, `snapshot`/`rollback`, `fetch` (netboot only). The system+tools block is the prefix-cache cut, so any change to the tool set invalidates every cached conversation. Hard rules that follow from this:
- No `bash`/shell tool, no dynamic tool registration, no MCP, no "model adds a tool". New capability = model writes a binary and `spawn`s it.
- No `ps` tool; the model reads `/proc` or writes its own.
- Skills are never put in the system prompt (would move the cache cut).

**Tool-call dialect.** Tools render through q27's existing qwen35 XML template path, `<tool_call>`/`<tool_response>` as vocab added tokens. No JSON function-calling shim. Arguments are flat strings/ints only (`spawn`'s `argv` is one string split by the harness). Parse tool calls from the completed turn, not the stream. Multiple calls per turn run sequentially, one response each, in order. Staying in this dialect is what keeps q27's 22-mode drift corpus protecting the parser; new drift shapes go into that corpus. `read` returns text or a hexdump page, never base64. Tool results truncate at ~4K tokens with a truncation marker. Compiler errors are normal tool responses.

**Sampler** is fixed: `--think --temp 1.0 --top-p 0.95 --top-k 20 --min-p 0.05 --think-budget 0`, `--constrain-tools` off. Not greedy (measured 0.511 vs 0.928).

**spawn modes**: `capture` (buffered, default timeout 60 s), `background` (logs to `/state/log/<pid>`), `tty` (child owns `/dev/tty1`; escape chord `Ctrl-]` twice kills its process group). Every child gets a cgroup v2 `potemkin/<pid>` with `memory.max` (default 512 MB) and `pids.max` (default 256); `q27-init` sets its own `oom_score_adj` to -1000. Harness writes `/state/procs` after every change. `rollback(id)` also kills every child spawned after that snapshot.

**compile + store.** `compile` is content-addressed, not `spawn(tcc)`: `/store/sha256:<hash>/{source.c,bin,manifest.json}`, with `/generated/bin/<name>` symlinked in. Only `c` in v1. The store is append-only and is the only source tree / reproducibility ledger.

**Filesystem.** Persistent: `/data` (not snapshotted), `/state` and `/generated` (snapshotted per turn via btrfs subvolumes, overlayfs on VM), `/store` (append-only), `/intent` (append-only log, never enforced). Everything else is read-only initramfs. Snapshot happens before any turn that calls `write`, `spawn`, or `compile`.

**Console.** Lines starting with `/` go to the harness, never the model. Fixed set: `/help /undo /procs /log /kill /skill /reboot`. No `/model`, `/settings`, `/clear`. A skill is `/generated/skills/<name>.md`, first line = description.

**Backends.** One image; kernel cmdline selects `llm=api|cuda|cpu` and `model=qwen|bonsai`. Qwen3.8-27B-MTP + DFlash2 drafter on 24 GB+ cards; Bonsai 2 27B (ternary, ~6 GB) below that, on CPU, and on Orin. `ip=dhcp` provides networking with no userland; `api_key=` authenticates netboot.

## Trusted base

Only kernel/initramfs, NVIDIA stack, libc + CUDA static libs, `q27-init`, weights + tokenizer, tcc + musl headers, rescue init, and certs (netboot only). Never add BusyBox, coreutils, a shell, `make`, `git`, `curl`, `python`, `systemd`, or a package DB to the image. The harness must not be Claude Code or any Node-based/shell-assuming agent.
