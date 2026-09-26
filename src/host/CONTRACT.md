# Contract: host primitives (`src/host`)

The eight tools the model sees, as a C++ library with no CUDA dependency.
Every argument is a string (flat, per the design doc); integers are parsed
from strings. `Host::call(name, args)` returns a `ToolResult` whose `body` is
the exact text that goes into `<tool_response>`. Tools never throw to the
caller: every failure is a body starting `error: `.

Paths the model passes are absolute and resolved under `Config::root`
(`""` in the image, a temp dir in tests). The eight names are frozen:
`read write stat spawn wait compile snapshot fetch`. Paired rows from the
design doc are one tool with a flag: `stat(list)`, `wait(signal)`,
`snapshot(rollback)`.

Verified by `build/test_host` (`tests/test_host.cpp`).

## Dispatch
- [x] Unknown tool name -> `error: unknown tool <name>` -> test `unknown_tool`
- [x] Missing required arg -> `error: <tool>: missing <arg>` -> test `missing_arg`
- [x] Non-integer where an integer is required -> `error:` -> test `bad_int`
- [x] Relative path -> `error:` (paths must be absolute) -> test `relative_path`

## read(path, offset="0", len="")
- [x] UTF-8 file returned verbatim -> test `read_text`
- [x] offset/len select a byte range -> test `read_range`
- [x] Body over `max_result_bytes` is cut and ends with a `[truncated: bytes A-B of N; read offset=B to continue]` line; `truncated=true` -> test `read_truncates`
- [x] Non-UTF-8 content returns a hexdump page headed `[binary: hexdump ...]`, never base64 -> test `read_binary_hexdump`
- [x] Missing file -> `error:` -> test `read_missing`
- [x] Files with no size (`/proc` style, st_size 0) are still read fully -> test `read_proc`

## write(path, content)
- [x] Creates parent directories -> test `write_creates_parents`
- [x] Overwrites, content byte-exact incl. newlines and `<`/`&` -> test `write_exact`
- [x] Body reports byte count -> test `write_exact`

## stat(path, list="0")
- [x] list=0: type, size, mode (octal), mtime for one path -> test `stat_file`
- [x] list=1: one line per entry, sorted, dirs suffixed `/`, symlinks shown `name -> target` -> test `stat_list`
- [x] Missing path -> `error:` -> test `stat_missing`

## spawn(exe, argv="", mode="capture", timeout_s="60", mem_mb="512", stdin="")
- [x] argv is one string split on whitespace, with `'` / `"` quoting; argv[0] is exe -> test `spawn_argv_split`
- [x] capture: returns `exit=<code>` then stdout/stderr, capped at `max_result_bytes` -> test `spawn_capture`
- [x] capture: `stdin` is fed to the child -> test `spawn_stdin`
- [x] capture: timeout kills the process group and reports `timeout` -> test `spawn_timeout`
- [x] Nonexistent exe -> `error:` (no zombie) -> test `spawn_missing_exe`
- [x] background: returns `pid=<n>` immediately; output goes to `/state/log/<pid>` -> test `spawn_background`
- [x] Every spawn/exit rewrites `/state/procs` (pid, mode, state, turn, argv) -> test `procs_table`
- [x] With `cgroup_root` set, child is placed in `<cgroup_root>/potemkin/<pid>` with `memory.max` and `pids.max` written (tested against a fake cgroup dir) -> test `spawn_cgroup_files`
- [x] tty mode: child gets its own pty, proxied to `Config::tty_path` (raw mode while proxying); output reaches the console and the last ~2K (ANSI stripped) comes back in the body -> test `tty_runs_child`
- [x] tty mode: console keystrokes reach the child -> test `tty_input_reaches_child`
- [x] tty mode: Ctrl-] twice kills the child's session, body says `escape` -> test `tty_escape_chord`
- [x] tty mode: a lone Ctrl-] is passed through -> test `tty_single_ctrl_bracket_passes_through`
- [x] tty mode: no default timeout (only an explicit timeout_s applies)

## wait(pid, signal="")
- [x] Waits for a background child, returns `exit=<code>` + tail of its log -> test `wait_background`
- [x] signal=TERM|KILL|INT|HUP|<num> sends it first -> test `wait_signal`
- [x] Unknown pid -> `error:` -> test `wait_unknown`

## compile(lang, name, source, opts="")
- [x] Only lang=c; anything else -> `error:` -> test `compile_lang`
- [x] Stores `/store/sha256:<hex>/{source.c,bin,manifest.json}`; `/generated/bin/<name>` symlinks to the bin -> test `compile_store_layout`
- [x] Hash covers lang+opts+source; same input twice -> second says `cached`, no recompile -> test `compile_cached`
- [x] manifest.json has name, turn, model, opts, timestamp, hash -> test `compile_store_layout`
- [x] Compiler error is a normal body: `exit=<n>` + compiler stderr, nothing stored -> test `compile_error`
- [x] Built binary runs via spawn -> test `compile_then_spawn`
- [x] name must be a plain filename (no `/`) -> test `compile_bad_name`
- [x] sha256 matches FIPS test vectors -> test `sha256_vectors`

## snapshot(rollback="")
- [x] No arg: copies `/generated` and `/state` into `snapshot_dir/<id>`, returns `snapshot=<id>`; ids increase -> test `snapshot_create`
- [x] rollback=<id>: restores both trees exactly (added files removed, changed files reverted) -> test `snapshot_rollback`
- [x] rollback kills children spawned after that snapshot -> test `rollback_kills_later_children`
- [x] Unknown id -> `error:` -> test `rollback_unknown`
(Copy-based backend; btrfs subvolumes replace it in pk-5vq.5.1.)

## fetch(url)
- [x] Present in the tool list only when `Config::netboot`; otherwise `error: fetch: not available offline` -> test `fetch_offline`
(Real HTTP lands in pk-5vq.3.4.)
