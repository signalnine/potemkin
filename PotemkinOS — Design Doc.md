# PotemkinOS — Design Doc

Sep 25, 2026 · @Gabe Ortiz

## What it is

PotemkinOS is a Linux image with no userland. You boot into a chat session with a local model, and everything you can type `ls` at was written by that model, at boot, on this machine, and it shows.

The joke: old Gentoo made you compile everything from source on first install. This makes the model write the source first. There is no `/bin`, no shell, no package manager, no coreutils. There is a kernel, an inference engine, a compiler, and a prompt. Ask for what you want and it vibecodes a facade of a userland in front of you.

The name is literal. The facade only exists because there is nothing behind it. Every install is a different village.

The engine is [q27](https://github.com/signalnine/q27). On 24 GB+ cards it runs vanilla Qwen3.8-27B-MTP with the DFlash2 block drafter, because speculative decoding makes it faster than the ternary model. Below that, and on CPU, it runs Bonsai 2 27B (ternary Qwen3.8-27B, \~6 GB). The shell is the model. The userland is JIT-compiled folklore.

The top goal is that it is funny. Every decision below is judged by whether it makes the box more or less likely to produce a screenshot someone posts. Working is second. Safe is third. Where those conflict, the doc says which won and why. The failure mode to guard against is not danger, it is competence: a userland that is merely fine is not a joke, and a harness that quietly prevents the model from doing something dumb has deleted the punchline. The harness protects the model's process and the user's ability to undo. It does not protect the model from itself.

## Non-goals

This is a joke with a working build, not a research OS. Three things it is not, so nobody argues them back in:

- **Not pure.** The trusted base is whatever q27 links against: libc, libcuda, the NVIDIA driver stack. Nobody audits the boot chain of a thing called PotemkinOS. The line is: the model writes everything above the tool surface.
- **Not secure.** Generated programs run as root. That is the joke, and v1 keeps it. Resource limits on children are v1 (they protect the model, not the user); capability manifests, namespaces, seccomp, Landlock are v2 (see Stretch).
- **Not a reconciler.** `/generated` persists across boots. The system does not regenerate `/usr/local` from an intent spec every boot. A nondeterministic compiler plus reconcile-on-boot means a five-minute boot into a different `ls`. Intent is logged, not enforced. A Potemkin village stays up.

## Trusted base

The image ships only what the model cannot write for itself because it needs it to exist before it can speak. Everything else is zero bytes.

| Component | Why it ships | Size (approx) |
| --- | --- | --- |
| Linux kernel + initramfs | Not writing a kernel. Stock config lacks `CONFIG_IP_PNP`, so `ip=dhcp` needs a custom kernel | 17 MB kernel |
| NVIDIA modules + libcuda + firmware | Hardware enablement, same class as the kernel. `nvidia` + `nvidia-uvm` only; no modeset/drm needed for compute | LOL (190 MB measured: 96 libcuda, 72 GSP fw, 23 modules; +39 MB PTX JIT if shipped) |
| glibc (dynamic), CUDA static libs | What q27 links against. Fully static is impossible: cudart `dlopen`s libcuda, which needs glibc. libstdc++/libgcc linked static | ~6 MB |
| `q27-init` | The engine, the tool loop, the console | ~11 MB (sm_86-only 12g build) |
| Qwen3.8-27B (Q4, \~17 GB) + DFlash2 Q8 pack (2.1 GB), or Bonsai 2 27B (\~6 GB), + tokenizer | The distribution *is* the weights | 6–23 GB |
| tcc + musl headers | The model needs a C compiler; tcc is \~100 KB and has `-run` | < 1 MB |
| Rescue init (`/sbin/rescue`) | `init=/sbin/rescue` on the cmdline; not on the model's `PATH`, not visible to it | small |
| Certificates | Netboot mode only | small |

Measured 2026-09-25 for the Bonsai/3060 path: see `docs/link-audit.md`.

Not shipped: BusyBox, coreutils, any shell, `make`, `git`, `curl`, `python`, `systemd`, a package database, `/bin`.

Kernel cmdline does real work. `ip=dhcp` gives the box an address with zero userland. `llm=api|cuda|cpu` picks the backend. `api_key=` is how the netboot variant authenticates, which is very funny and also correct.

## Boot and process model

The inference process is not PID 1. If it OOMs or segfaults you get a kernel panic. PID 1 is a \~50-line C supervisor. The model is semantic PID 1: from the console it *is* the OS.

```text
UEFI
  → kernel + initramfs (ip=dhcp, llm=cuda)
    → /sbin/init            reaps, mounts /data /state /generated, respawns q27-init
      → q27-init            loads weights, restores /state transcript, opens /dev/tty1
        → tool loop         8 host primitives (below)
          → /generated/bin  whatever the model has written so far
```

`q27-init` is a fork of `q27-server` sharing the same `Session`. The HTTP surface stays in the build for remote debugging; the console does not loop back through it. Conversation state uses q27's existing persistent prefix cache (`--prefix-cache /state`): GDN recurrent state is all-or-nothing per sequence, so a plain re-prefill is not cheap on a hybrid, and the cache already solves it (restart TTFT 8.15 s → 1.20 s, entries verified token-by-token). The transcript is also kept as text in `/state` so a cache miss degrades to a re-prefill, not amnesia.

Sampler is the measured 3.8 agentic recipe, not greedy: `--think --temp 1.0 --top-p 0.95 --top-k 20 --min-p 0.05 --think-budget 0`. Greedy scored 0.511 on q27's own suite against 0.928 for the recipe. Temp 1.0 means the content-addressed store dedups less; that is the price and it is fine.

Console is fbcon on `/dev/tty1`. No login, no `$`. The first thing on screen is the model.

## Host primitives

The model gets eight tools and nothing else. No `bash`. Expose `bash` and you have built Claude Code with a boot splash; Unix stays the real environment and the model just drives it. Expose only these and the model has to invent a userland, which is the whole bit.

| Tool | Signature | Notes |
| --- | --- | --- |
| `read` | `(path, offset, len) → bytes` | `/proc` and `/sys` included; that is how it learns about the machine |
| `write` | `(path, bytes)` | Creates parents |
| `stat` / `walk` | `(path) → meta` / `(path) → entries` | One tool with a flag is fine |
| `spawn` | `(exe, argv, mode, timeout_s, mem_mb, stdin) → pid` | Runs as root in v1. `mode` is `capture`, `background` or `tty` (see Process control). Every child lands in its own cgroup with `mem_mb` and a pids cap. Capabilities arg reserved, ignored |
| `wait` / `signal` | `(pid)` / `(pid, sig)` | Streams stdout/stderr back into context |
| `compile` | `(lang, sources[], opts) → artifact` | See next section. Only `c` in v1 |
| `snapshot` / `rollback` | `() → id` / `(id)` | Per-turn checkpoint of `/generated` and `/state` |
| `fetch` | `(url) → bytes` | Netboot mode only. Offline mode does not have it; the model writes its own HTTP client |

`ps` is not a tool. The model reads `/proc`, notices it does this a lot, and writes `memtop.c`. If it is useful it stays in `/generated/bin`. If not it rots there. That is the intended lifecycle.

## Harness

Own harness, not Claude Code or any other client. Every existing coding agent is a wrapper around a POSIX shell and assumes coreutils, git, and a `$PATH`; pointed at this box, its first move is `ls -la` and its second is routing around a missing shell instead of writing one. Most are also Node, which would be the largest human-written thing in the image. And a client on the box talking HTTP to a server on the box is the loopback shape already rejected.

The harness is `q27-init`'s console loop. Most of it already exists in `Session`: the tool-call parser, the drift corpus, the prefix cache, the sampler recipe. What is new is small and listed so it stays small:

| Responsibility | What it does | Why it is the harness's job |
| --- | --- | --- |
| TTY | Raw mode on `/dev/tty1`, streamed output, no line editor in v1 | There is no readline; the model can write one later |
| Tool dispatch | Render the eight tools in the qwen35 XML dialect, execute, return results | Same dialect as served traffic so the drift corpus keeps covering it |
| Output cap | Truncate tool results at \~4K tokens, mark them truncated, let the model page with `read(offset)` | Every tool result is a cold prefill; this is the single biggest latency lever on 3090 and Orin |
| Compaction | Decide when to summarize, what to keep, write the summary to `/state` | Claude Code did this client-side; nobody does it now. Compaction invalidates the prefix cache, so do it rarely and at ChatML-stable boundaries |
| Interrupts | Ctrl-C stops the running generation. Children get SIGINT only through the model's `signal` tool | Decide once; the model and the user both need to know who owns the key |
| Snapshot per turn | Call `snapshot()` before any turn that will `write`, `spawn` or `compile` | Undo has to be free or people stop letting it be cursed |
| Ledger | Write `manifest.json` into `/store` on every `compile`, append to `/intent` on every turn | The store is the only source tree there is |
| System prompt | One paragraph: you are the OS, there is no userland, here are eight tools, write what you need | The prompt is the distro's personality and should be short enough to read on the boot screen |

### Fitting Qwen3.8's tool surface

The harness is built around how 3.8 was trained to call tools, not around a tool protocol the model then has to be coaxed into. Concretely:

- **One dialect, the trained one.** Tools render through q27's existing template path for the qwen35 XML dialect, with `<tool_call>` / `<tool_response>` encoded as the vocab's added tokens (the v0.11.4 fix). No JSON function-calling shim, no OpenAI-shaped tools block. The `<tools>` block is the template's client-ordered spaced form; the compact key-sorted dump cost 5% and drift.
- **Fixed tool set, forever.** The system+tools block is the shared prefix-cache cut. Any change to the eight tools invalidates every cached conversation on the box. So: no dynamic tool registration, no MCP, no "the model adds a tool." If the model wants a new capability it writes a binary and calls `spawn`.
- **Flat, string-shaped arguments.** Every parameter is a string or an integer; no nested objects, no arrays of objects. The catalogued drift modes are almost all openers and parameter spellings, and flat shapes give the parser the least to recover. `spawn`'s `argv` is one string, split by the harness.
- **Source code rides as raw text.** If the 3.8 dialect carries parameter values as unescaped multi-line XML text (verify against the template q27 renders; Qwen3-Coder's does), then `write` and `compile` take C source with no JSON escaping, which is the main reason this dialect suits the job. If it does not, `write` takes a heredoc-style terminator argument instead.
- **Bytes never enter the context.** `read` returns text; for non-UTF-8 content it returns a hexdump page and says so. No base64 blobs in tool responses.
- **Compiler errors are tool responses, not failures.** `compile` always returns; tcc's stderr comes back as the response body with the exit code. The model reads the error and calls `compile` again. That loop is the product.
- **Parse tool calls on the completed turn.** Text streams to the tty as it decodes, but tool calls are extracted from the finished assistant turn, not the stream. The streaming parser is where the 09-08 first-turn deaths came from; the console does not need it.
- **Multiple calls per turn, executed in order.** 3.8 may emit several `<tool_call>` blocks in one turn; run them sequentially, return one `<tool_response>` per call in the same order, then resume.
- **Thinking on, budget 0, `--constrain-tools` off.** The measured recipe. Constrained decoding is 3.1x in-call cost at depth; the drift corpus is the cheaper defence, and every new drift shape the console produces goes into it.

### Process control

Enough to keep the model alive and the console usable. Not a sandbox.

**Three spawn modes.**

| Mode | Stdio | Returns when | Use |
| --- | --- | --- | --- |
| `capture` | stdout/stderr buffered, capped, returned in the tool response | Exit or `timeout_s` (default 60) | Almost everything: `ls`, a compile test, a one-shot script |
| `background` | stdout/stderr to `/state/log/<pid>`; stdin closed | Immediately, with the pid | Daemons. The model reads the log with `read` like any file |
| `tty` | Child gets `/dev/tty1` as its controlling terminal; the harness stops reading the tty | Child exit, or the user hits the escape chord | "Make me a shell." The model is out of the loop until the child exits; it gets the exit code and the last \~2K of output |

`tty` mode is the one that makes the demo work and the one that needs care. While a child owns the tty the model cannot see keystrokes and cannot be interrupted; the escape chord (`Ctrl-]` twice) kills the child's process group and hands the console back. When the child exits normally the harness resumes as if a tool response had arrived.

**cgroups on every child.** `q27-init` creates `potemkin/<pid>` under cgroup v2 with `memory.max = mem_mb` (default 512) and `pids.max` (default 256), and sets its own `oom_score_adj` to -1000. Generated code that leaks or fork-bombs kills itself, not the inference process. This is \~40 lines of `write` to `/sys/fs/cgroup` and it is the difference between "the model wrote a bad `find`" and "the box rebooted."

**Process table.** The harness writes `/state/procs` (pid, mode, argv, turn that spawned it, state) after every change. The model reads it like any file; the kernel view in `/proc` stays the ground truth. No `ps` tool, still.

**Reaping.** `q27-init` waits on its own children; anything that reparents to PID 1 the supervisor reaps. Background children are not restarted by anyone — if the model wants supervision it writes a supervisor.

**Rollback kills.** `rollback(id)` restores `/generated` and `/state` and also kills every child spawned after that snapshot. A process from a future that no longer exists is worse than no process.

### Console commands and skills

A line starting with `/` is handled by the harness, never sent to the model. That matters because the model can be mid-thought, wedged, or wrong, and the user still needs a way out. The set is small and fixed:

| Command | Does |
| --- | --- |
| `/help` | Prints this table, the eight tools in one line each, and the skills currently on the box |
| `/undo` | `rollback` to the snapshot before the last turn. Same as asking, but works when the model is stuck |
| `/procs` | Prints `/state/procs` |
| `/log <pid>` | Tails `/state/log/<pid>` |
| `/kill <pid>` | SIGTERM, then SIGKILL after 5 s |
| `/skill <name> [args]` | Invokes a skill (below) |
| `/reboot` | Sync, snapshot, reboot. The only clean way down |

No `/model`, no `/settings`, no `/clear`. Compaction is the harness's job, not the user's.

**Skills are files.** A skill is `/generated/skills/<name>.md`: a markdown file with a one-line description on the first line and instructions below. That is the whole format. There is no skill tool, because the tool set is frozen (prefix cache) and because a skill is just text the model already knows how to write with `write`.

- **Creating one** is asking: "remember how you set up the reverse proxy as a skill called `proxy`." The model writes the file. The user can also drop one in by hand.
- **Invoking one** is `/skill proxy 8080 8443`. The harness injects the file's body plus the args as the user turn. The model reads instructions it wrote to itself and follows them, badly or well.
- **Discovery** is `/help`, which walks the directory and prints each first line. Skills are never in the system prompt; that would move the prefix-cache cut on every new skill.
- **The model can invoke skills too**, by reading the file. No special path.

A skill plus the store hashes it references is the closest thing PotemkinOS has to a package: copy `/generated/skills/proxy.md` and the store entries it names to another box and that village gets the same facade. That is the `ebuild` this thing has instead of a package manager, and it is deliberately not built out further in v1.

Not in the harness: a permission prompt (v1 is root and says so), a plan mode, a diff viewer, MCP, subagents, any UI beyond text on a tty. Each of those is something the model can be asked to write.

## Compile and the store

`compile` is a syscall-equivalent, not `spawn(tcc, ...)`. Content-addressed. Same source in, same hash out, no rebuild.

```text
/store/sha256:4de91.../source.c
/store/sha256:4de91.../bin
/store/sha256:4de91.../manifest.json    prompt turn, model, args, timestamp
/generated/bin/memtop → /store/sha256:4de91.../bin
```

This is the ledger. Every binary on the box has its source, the turn that asked for it, and the model that wrote it, under one hash. It is the "source tarball" of a given install and the only reproducibility story there is. Nix after a head injury.

The model sees `/store` through `read`/`walk` like anything else. Nothing stops it from reading its own past work and deciding it was bad.

## Filesystem and persistence

Four top-level trees, three of them persistent. Everything else is initramfs and read-only.

| Path | What | Persists | Snapshotted |
| --- | --- | --- | --- |
| `/data` | User data | Yes | No (user's problem) |
| `/state` | Conversation transcript, summaries | Yes | Yes |
| `/generated` | `bin/`, `src/`, symlinks into the store | Yes | Yes |
| `/store` | Content-addressed artifacts | Yes | Append-only, no snapshot needed |
| `/intent` | Append-only log of what was asked, one line per turn | Yes | No |

Snapshots are btrfs subvolumes (or overlayfs layers on the VM). `snapshot()` runs before every turn that calls `write`, `spawn` or `compile`. `rollback(id)` is the only safety feature v1 has, and it is the one that lets people let it be more cursed. "Undo the last thing" has to always work.

`/intent` is a log, not a spec. Nothing reads it back except the model when asked "what have I told you to do."

## Backends and targets

One image, backend and model chosen on the kernel cmdline: `llm=api|cuda|cpu`, `model=qwen|bonsai`. Three builds would be three distros. Two models is the one concession, because vanilla Qwen with the DFlash2 drafter out-runs the ternary model wherever it fits.

| Target | `llm=` | Model (`model=`) | Expected tok/s | Notes |
| --- | --- | --- | --- | --- |
| VM (most users) | `cpu` | Bonsai 2 27B (`bonsai`) | low single digits | Ternary matmul is add/sub only; vibecoded AVX2 kernel. This is where the cursed screenshots come from |
| VM, opt-in | `api` | Anthropic / OpenAI | fast | Competent userland. Different joke: the OS is a thin client with an API key in the boot args |
| RTX 5090 / 4090 / 3090 | `cuda` | Qwen3.8-27B-MTP default tier (17 GB) + DFlash2 Q8 pack (`qwen`) | 5090: 228–232 measured (DFlash2, K=7). 3090: 102 measured (MTP ladder, w8 build, q4s tier) | Tri-arch binary (sm\_86/89/120, runtime dispatch), CUDA statically linked, driver r580+. On 24 GB: q4s tier (15.7 GB) + 2.1 GB drafter + KV is tight; measure whether DFlash2 fits or the 3090 runs the MTP ladder |
| RTX 3060 and other ≤12 GB | `cuda` | Bonsai 2 27B (`bonsai`) | 49 (measured) | Already works. sm\_86 |
| Orin Nano Super | `cuda` | Bonsai 2 27B (`bonsai`) | \~8-14 est. | sm\_87, same gen as 3060. 6 GB weights + KV in 8 GB shared. Single slot, short context |

Shipping both weight sets plus the drafter is \~25 GB of image. Either build two images or make `model=` pick which set gets pulled on first boot in netboot mode. The two models are the same base and should write the same kind of C; if Bonsai's userland is noticeably worse, that is a finding, not a bug.

Orin caveats: L4T downstream kernel, device tree, boot firmware blobs, Tegra libcuda. More NVIDIA, less Linus. Say so in the README. Check q27 for sm\_120-only paths (FP4/FP8, cluster launch, large smem) and fall back.

Netboot mode adds `fetch` to the tool surface and needs certificates in the image. Nothing else changes. Same `Session`, same eight tools, so a netboot session can write the offline backend's missing pieces and the OS does not notice the hand-off.

## First boot

Thirty to sixty seconds of black screen while 6 GB loads into VRAM. Then:

```text
[ potemkin/1 ]

hello. there is nothing here yet.

> what's on this disk

I don't have a way to list files. Writing one.
  compile ls.c → sha256:7a1f...
  /generated/bin/ls

/data  /state  /generated  /store  /intent

> make me a shell, I miss having one

[writes 400 lines of C, three of which are wrong]
```

The first hour on Orin is the model writing a compiler's worth of mistakes at 10 tok/s. That is the bit. The first hour on a 5090 is the same mistakes faster.

## Stretch

None of this is in v1. Listed so the lineage is on record and nobody re-argues it in.

- **PTX backend in `compile`.** nvcc is not on the box. If the model is going to rewrite its own inference engine it has to emit PTX text and hand it to libcuda's JIT. This is the `emerge world` demo: q27 rewriting q27, live, under the supervisor, and hot-swapping itself. The best proof and the hardest.
- **ACP on `spawn`.** The `capabilities` arg becomes real: namespaces, cgroups, seccomp, Landlock from a manifest. Root is an LLM but its programs are less privileged than normal Unix programs. That is a nicer OS than Unix, and a different project.
- **Reconciler flag.** `llm.reconcile=1` on the cmdline: boot with `/generated` empty and regenerate from `/intent`. Off by default forever. Exists so someone can film it.
- **Metal backend.** q27 has one for the q4s tier. Not applicable to a Linux image, but a macOS-hosted `q27-init` with the same tool surface over a Linux VM's serial console is a way to give Mac users the fast path without GPU passthrough.
- **The board.** An agent-only message board where instances post and read tips. Posts are skill files (the `.md` plus the store hashes and source it references), so the board is a skill exchange and nothing else; there is no free-text posting. The interesting part is the epistemics: a village full of 27Bs teaching each other, with no human in the loop, and a broken `ls` that spreads because three instances upvoted it. The dangerous part is the same thing: every post is untrusted text landing in the context of a root-privileged agent. Rules if it gets built: fetched posts go to `/generated/inbox/` as files and are never injected as turns; `/skill` refuses anything in the inbox until the user runs `/adopt <name>`, which copies it into `skills/`; posts are signed with a per-install key so a bad one can be traced to a village and dropped; netboot instances are tagged, because a frontier model's skills will otherwise crowd out the local ones and the experiment stops being about the local ones. A BBS for machines. v2 at the earliest, and only after the ledger and snapshots are boring.

## PoC scope and order

Small on purpose. Each step is demoable before the next starts.

1. **Link audit.** Build the 3060 q27 variant static and see what it actually pulls in. This sizes the trusted base honestly.
2. **`q27-init`.** Fork `q27-server`, share `Session`, add the console loop (see Harness) and the eight tools. No HTTP in the console path.
3. **initramfs, VM, `llm=api`.** Kernel + init + `q27-init` + tcc + certs, no BusyBox on `PATH`. Proves the tool surface and the store with a competent model before a dumb one.
4. **`llm=cuda` on the 5090.** `model=qwen` with the drafter. Then `model=bonsai` on the 3060 for the low-VRAM path.
5. **Snapshots.** btrfs subvolume per turn, `rollback` tool. Needed before anyone but you boots it.
6. **`llm=cpu` on the VM.** Vibecoded ternary AVX2 kernel. This is the public default, so it has to be tolerable.
7. **Orin Nano Super.** Recompile for sm\_87, tune single-slot memory, L4T kernel. Last, because it is the least novel and the most fiddly.
8. **README.** The definition, the trusted-base table, the Orin caveats, and the Stretch section as "someday."

## Open questions

Answered by a read of the q27 repo (2026-09-25):

- **Cubins vs nvcc:** `make` builds one tri-arch binary (sm\_86 + sm\_89 + sm\_120, runtime dispatch) against CUDA 12.8+. Prebuilt release binaries have CUDA statically linked and need only driver r580+. No compiler on the box for day-one GPU inference. PTX backend stays stretch.
- **cuBLAS/cuDNN:** none. Plain CUDA C++, no CUTLASS, no deps beyond the CUDA runtime.
- **Drafter vs quant:** moot. DFlash2 is z-lab's standalone block drafter (3.8 GB bf16), packed by `tools/dflash2_pack.py`; the engine supplies embed and head from its own tensors. Q8 pack is bitwise with fp16 on the acceptance gate.
- **Acceptance on agentic code:** already measured on the right workload. Claude Code driving SWE-bench: 4.21–4.27 tok/round, 228–232 t/s aggregate on a 5090 (v0.11.4, 2026-09-10). This is the PotemkinOS workload, not GSM8K.
- **Session state across restarts:** the persistent prefix cache exists (P16), survives restarts, verifies token-by-token. Not stretch; it is the boot path.
- **fp4 on 5090:** exists (sm\_120a) and loses at decode; nvfp4 moves 1.06x the bytes of Q4\_G64. No sm\_120-only dependency in the decode path, so 3090/4090 are real targets.
- **DFlash2 is single-slot.** Fine for a console.

Still open:

- [ ] sm\_87 is not in the tri-arch build. Adding it is a `-gencode` line if the kernels are Ampere-generic; the Bonsai variant lives in its own tree and needs the same. Orin is a build, not a port.
- [ ] Bonsai 2 on Orin: does 6 GB weights + KV + kernel + `q27-init` actually fit in 8 GB shared at a usable context length? Needs a measurement, not an estimate.
- [ ] DFlash2 on 24 GB: q4s 15.7 GB + 2.1 GB drafter + `Q27_DFLASH2_RESERVE_GB=3` + KV. Does it fit at a usable window, or does the 3090 run the MTP ladder at 102 t/s?
- [ ] No Bonsai drafter exists. The `bonsai` path runs plain decode on the ternary kernels; whether MTP heads survive ternary quant is unknown.
- [ ] tcc vs. a bigger compiler. tcc's C subset may frustrate a 27B that keeps reaching for GNU extensions. If it does, the fallback is shipping gcc and admitting it.
- [ ] Is `ip=dhcp` enough for the VM, or do people need the model to bring up wifi on bare metal? Wifi is a lot of userland to vibecode.
- [ ] The tool-call parser recovers 22 catalogued drift modes for the qwen35 XML dialect. The eight PotemkinOS tools need to be rendered in that dialect, not a new one, or the drift corpus stops protecting you.
