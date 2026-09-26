# q27 in-process API map (for q27-init)

Mapped 2026-09-25 against q27-master `8be624e` (v0.14.1). Paths relative to the
q27 repo. The design doc's "Session" is not a type; it is this set of calls.

## Skeleton to copy
`tools/constrain_e2e.cu` (142 lines) is an in-process driver: tokenizer ->
`chatml_prompt` -> `Engine` -> `generate` with hooks. Its build comment is
stale (missing `vgemm.cu dflash2.cu pf4.o`). The console loop should copy the
non-stream routed_chat body in `server.cu:2661-2808`.

## Boot (single slot, no conductor)
1. `setenv(..., 0)` profile defaults before constructing the Engine
   (`server.cu:514-584`): sm_80-88 `Q27_KV=turbo5k`, sm_89+ `Q27_KV=fp8`;
   `Q27_FD=mma Q27_PMIN=0.5 Q27_MAXD=auto7 Q27_SUFFIX=1 Q27_SUFFIX_W=<W_MAX>`;
   **`Q27_PF_BATCH_MIN=2`** (otherwise short prompts clear the snapshot ring).
2. `q27::Tokenizer tok(path)`; `think_close_ids = tok.encode("</think>\n\n")`.
3. `Model::open` -> `validate_arch` -> **`set_tool_dialect_for_model(meta_json)`**
   (picks the XML dialect for qwen38) -> `DeviceModel dm(m)`;
   `dm.upload_all(q27k::pf4_on()); dm.checksum_baseline();` (`server.cu:735-748`).
4. Auto-ctx (`server.cu:814-871`): `per_tok = 17*pair` (turbo5k 1056 B, fp8 2048),
   `fixed = Q27_FIXED_STACK_GB*1e9` (read only by server.cu), slack 0.15e9 when
   that is set; `ctx = min(cap, (free-fixed-slack)/per_tok)` floored to 4096.
5. `Engine e(m, dm, ctx)` (borrowing ctor, `engine.cuh:1075`, pool/arena null);
   `e.fast_head=...; e.capture_constrained=false; e.build_graph(); e.build_spec_graphs();`
6. `EOS = tok.eos()` (`<|im_end|>`).

## Render
`struct Msg {role, content, reasoning}` (`api_common.h:197`). Tool results are
`user` messages built with `tool_response_text(out)`. Assistant history with
calls: `assistant_content_38(text, {tool_call_xml_ordered(name, ordered_json args)})`.

`chatml_prompt(msgs, tools_json, think, &stable_off, &sys_off, {}, nullptr, &topts)`
(`api_common.h:729`) returns text. `topts.tools_decl` = concat of
`"\n" + ordered_dump_spaced(tool)` per tool (client-ordered spaced form);
still pass the sorted `json tools`. Tools are OpenAI-shaped
`{"type":"function","function":{name,description,parameters}}`.
Goldens: `tools/golden/qwen38_*`.

Encode in three pieces `[0,sys_off) [sys_off,stable_off) [stable_off,end)`;
`stable_len` = tokens up to stable_off (`server.cu:2503-2517`).

## Generate
`e.samp = {inv_temp=1/temp, top_p, seed, top_k, min_p}`; `e.pfx_sys_len = sys_len`;
`int n = e.generate(prompt, n_max, EOS, [&](int id){...; return true;}, stable_len, &bt);`
(`engine.cuh:5833`). EOS not passed to the callback; no stop strings. Clamp
`n_max <= max_ctx - prompt - (ctx_round_reserve()-1)`. `e.gs` has stats.
Per token: `sp.feed(ugate.feed(tok.decode_one(id)))` with `StreamSplitter sp`
(seed `sp.chan = THINK` when the prompt ends in `<think>\n`) and `Utf8Gate`;
flush both at the end. Copy `route()` from `server.cu:2668-2681` exactly.

KV reuse across turns needs: the same Engine object, byte-identical render up
to the previous `stable_off` (same system prompt, tools, TemplateOpts, prior
messages), and the split encode above. The snapshot sits before the assistant
opener, so generated tokens are re-prefilled next turn.

## Parse tool calls (completed turn)
`unfinished_tool_wrapper` -> `take_unclosed_final_tool_segment` ->
`resolve_ordered_tool_segments(segments, &tools, n<n_max && !bt.budget_truncated, eligible)`
-> `recover_unclosed_tool_tail` (`server.cu:2749-2797`). Result:
`.text`, `.reasoning`, `.calls` (`ToolCall{ok,name,json arguments,raw,...}`).
**Never set `Q27_TOOL_STRICT=1`**: strict mode rejects every XML call.

Parameter values are raw multi-line text, no escaping (`api_common.h:683-687`,
`3084-3318`); one leading and one trailing newline stripped. Gotcha: values are
`json::parse`d, so `content` of `42`/`true`/`null` arrives non-string; coerce
with `v.is_string() ? v.get<std::string>() : v.dump()`.

## Prefix cache
`PrefixCacheCfg{root, max_bytes=20GiB, min_tokens=4096, max_tokens=32768, step=8192}`.
Compat hash includes the model path string as given. Declare the cache before
the Engine. `eng.pcache=&pc; eng.pram=&pr; eng.pfx_prealloc(max_tokens)`.
Entries are written during prefill (system block and stable prefix), only once
`L >= min_tokens`; restored only when snapshot and checkpoint ring both miss.

## Build
Replace `src/server.cu` in the 12g line with our main:
`nvcc -O2 -std=c++17 -gencode arch=compute_86,code=sm_86 -Xcompiler -Wall -DQ27_W_MAX=8 -DQ27_PF_T=256 -Xcompiler -pthread <main.cu> src/{dflash2,blocks,prefill,kernels,spec3,vgemm}.cu src/device_model.cu src/loader.cpp src/tokenizer.cpp build/pf4.o`.
`pf4.o`: `nvcc -O2 -std=c++17 -gencode arch=compute_120a,code=sm_120a -c src/pf4.cu`.
No externs required. Optional copies from server.cu: `parse_sample`,
`ReasoningBudgetObserver` (think budgets), `HookGuard`.
