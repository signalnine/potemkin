// q27 in-process backend: the server's non-stream routed_chat path
// (server.cu:2661-2808) with the HTTP taken out. See docs/q27-api.md.
#include "backend_q27.h"

#include "engine.cuh"
#include "api_common.h"
#include "prefix_cache.h"
#include "tokenizer.h"

#include <filesystem>
#include <random>

namespace pk {
namespace {

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;
using Chan = q27::StreamSplitter::Chan;

// Parameter order as the model wrote it, so history renders back the same way.
std::vector<std::pair<std::string, std::string>> ordered_args(const q27::ToolCall& c) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!c.arguments.is_object()) return out;
    std::vector<std::pair<size_t, std::string>> keys;
    for (auto it = c.arguments.begin(); it != c.arguments.end(); ++it) {
        size_t pos = c.raw.find("<parameter=" + it.key() + ">");
        keys.emplace_back(pos == std::string::npos ? SIZE_MAX : pos, it.key());
    }
    std::stable_sort(keys.begin(), keys.end());
    for (auto& [pos, k] : keys) {
        const json& v = c.arguments[k];
        out.emplace_back(k, v.is_string() ? v.get<std::string>() : v.dump());
    }
    return out;
}

class Q27Backend : public Backend {
public:
    explicit Q27Backend(const Q27Opts& o) : o_(o) {
        int dev = 0, mj = 0, mn = 0;
        cudaGetDevice(&dev);
        cudaDeviceGetAttribute(&mj, cudaDevAttrComputeCapabilityMajor, dev);
        cudaDeviceGetAttribute(&mn, cudaDevAttrComputeCapabilityMinor, dev);
        int cc = mj * 10 + mn;
        // The server's CC-profile defaults (server.cu:514-584). User env wins.
        if (cc >= 89) { setenv("Q27_KV", "fp8", 0); setenv("Q27_FD", "mma", 0); }
        else if (cc >= 80) { setenv("Q27_KV", "turbo5k", 0); setenv("Q27_FD", "mma", 0); }
        setenv("Q27_PMIN", "0.5", 0);
        setenv("Q27_MAXD", "auto7", 0);
        setenv("Q27_SUFFIX", "1", 0);
        setenv("Q27_PF_BATCH_MIN", "2", 0);
        setenv("Q27_SUFFIX_W", std::to_string(W_MAX).c_str(), 0);

        tok_ = std::make_unique<q27::Tokenizer>(o.tok);
        model_ = std::make_unique<q27::Model>(q27::Model::open(o.model));
        q27::set_tool_dialect_for_model(model_->meta_json);
        dm_ = std::make_unique<q27::DeviceModel>(*model_);
        dm_->upload_all(q27k::pf4_on());
        dm_->checksum_baseline();

        int ctx = o.ctx;
        if (ctx < 0) {
            size_t free_b = 0, total_b = 0;
            cudaMemGetInfo(&free_b, &total_b);
            const char* kv = getenv("Q27_KV");
            double pair = !kv ? 4096 : !strcmp(kv, "fp8") ? 2048 : !strcmp(kv, "turbo5k") ? 1056
                        : !strcmp(kv, "turbo3") ? 800 : !strcmp(kv, "int8g64") ? 1456 : 4096;
            double per_tok = 17.0 * pair;
            long budget = (long)((double)free_b - o.fixed_stack_gb * 1e9 - 0.15e9);
            long c = budget > 0 ? (long)(budget / per_tok) : 0;
            long cap = (kv && strcmp(kv, "fp16")) ? 262144 : 131072;
            ctx = (int)(std::min(c, cap) / 4096 * 4096);
            if (ctx < 4096) ctx = 4096;
        }
        if (!o.prefix_cache.empty()) {
            q27::PrefixCacheCfg pc;
            pc.root = o.prefix_cache;
            pcache_ = std::make_unique<q27::PrefixCache>();
            pcache_init(pc);
        }
        eng_ = std::make_unique<Engine>(*model_, *dm_, ctx);
        eng_->fast_head = true;
        if (pcache_ && pcache_->enabled()) eng_->pcache = pcache_.get();
        eng_->capture_constrained = false;
        eng_->build_graph();
        eng_->build_spec_graphs();
        if (pcache_ && pcache_->enabled()) eng_->pfx_prealloc(q27::PrefixCacheCfg{}.max_tokens);

        std::string body = std::string("{\"tools\": ") + kToolsJson + "}";
        tools_ = q27::openai_tools_json(json::parse(body));
        decl_ = q27::openai_tools_decl(body);
        name_ = "q27:" + o.model.substr(o.model.rfind('/') + 1);
        std::random_device rd;
        seed_ = ((unsigned long long)rd() << 32) | rd();
    }

    int context_limit() const override { return eng_->max_ctx; }
    std::string name() const override { return name_; }

    GenResult generate(const std::vector<Message>& msgs, StreamSink& sink,
                       const std::atomic<bool>& cancel) override {
        GenResult res;
        std::vector<q27::Msg> qm;
        for (auto& m : msgs) {
            if (m.role == "tool") {
                qm.push_back({"user", q27::tool_response_text(m.content)});
            } else if (m.role == "assistant") {
                std::vector<std::string> calls;
                for (auto& c : m.calls) {
                    if (!c.ok) { calls.push_back(c.raw); continue; }
                    ojson a = ojson::object();
                    for (auto& [k, v] : c.args) a[k] = v;
                    calls.push_back(q27::tool_call_xml_ordered(c.name, a));
                }
                qm.push_back({"assistant", q27::assistant_content_38(m.content, calls), m.reasoning});
            } else {
                qm.push_back({m.role, m.content});
            }
        }

        size_t stable_off = 0, sys_off = 0;
        q27::TemplateOpts topts;
        topts.tools_decl = decl_;
        std::string rendered = q27::chatml_prompt(qm, tools_, o_.think, &stable_off, &sys_off, {}, nullptr, &topts);
        std::vector<int> prompt;
        int sys_len = 0;
        size_t sys_cut = (pcache_ && sys_off > 0 && sys_off <= stable_off) ? sys_off : 0;
        if (sys_cut) { prompt = tok_->encode(rendered.substr(0, sys_cut)); sys_len = (int)prompt.size(); }
        auto mid = tok_->encode(rendered.substr(sys_cut, stable_off - sys_cut));
        prompt.insert(prompt.end(), mid.begin(), mid.end());
        int stable_len = (int)prompt.size();
        auto tail = tok_->encode(rendered.substr(stable_off));
        prompt.insert(prompt.end(), tail.begin(), tail.end());
        res.prompt_tokens = (int)prompt.size();

        int cap = eng_->max_ctx - (int)prompt.size() - (eng_->ctx_round_reserve() - 1);
        int n_max = std::min(o_.n_max, cap);
        if (n_max <= 0) { res.end = "ctx-guard"; return res; }

        q27k::SampleParams sp{};
        sp.inv_temp = o_.temp > 0 ? 1.0f / o_.temp : 0.0f;
        sp.top_p = o_.top_p;
        sp.top_k = o_.top_k;
        sp.min_p = o_.min_p;
        sp.seed = seed_++;
        eng_->samp = sp;
        eng_->pfx_sys_len = sys_len;

        q27::StreamSplitter split;
        q27::Utf8Gate ugate;
        Engine::DecodeTask bt;
        std::vector<std::pair<Chan, std::string>> segments;
        auto route = [&](Chan ch, const std::string& t) {
            if (t.empty()) {
                if (ch != Chan::TOOL && !segments.empty() && segments.back().first == Chan::TOOL)
                    segments.emplace_back(ch, std::string());
                return;
            }
            if (ch == Chan::THINK) sink.think(t);
            else if (ch == Chan::TEXT) sink.text(t);
            if (!segments.empty() && segments.back().first == ch) segments.back().second += t;
            else segments.emplace_back(ch, t);
        };
        if (o_.think) split.chan = Chan::THINK;
        int n = eng_->generate(prompt, n_max, tok_->eos(), [&](int id) {
            for (auto& [ch, t] : split.feed(ugate.feed(tok_->decode_one(id)))) route(ch, t);
            return !cancel.load();
        }, stable_len, &bt);
        for (auto& [ch, t] : split.feed(ugate.flush())) route(ch, t);
        bool incomplete = q27::unfinished_tool_wrapper(n, n_max, bt.budget_truncated, split.chan);
        for (auto& [ch, t] : split.flush()) route(ch, t);
        std::string unclosed = q27::take_unclosed_final_tool_segment(segments, incomplete);
        auto ordered = q27::resolve_ordered_tool_segments(
            segments, &tools_, n < n_max && !bt.budget_truncated,
            [](const std::string&, size_t) { return true; });
        ordered.recovered += q27::recover_unclosed_tool_tail(
            unclosed, &tools_, [&](const std::string& t) { ordered.append_visible_text(t); },
            [&](q27::ToolCall c) {
                if (!c.ok) return false;
                ordered.append_tool_call(std::move(c));
                return true;
            });

        res.text = ordered.text;
        res.reasoning = ordered.reasoning;
        for (auto& c : ordered.calls) {
            ToolCallRec r;
            r.name = c.name;
            r.ok = c.ok;
            r.raw = c.raw;
            if (c.ok) r.args = ordered_args(c);
            res.calls.push_back(std::move(r));
        }
        // A call the parser gave up on is still a call the model meant to make:
        // hand it back as an error so the model can try again.
        if (res.calls.empty() && !unclosed.empty()) {
            ToolCallRec r;
            r.ok = false;
            r.raw = unclosed;
            res.calls.push_back(r);
        }
        res.gen_tokens = n;
        res.end = cancel.load() ? "cancelled" : eng_->gs.end;
        return res;
    }

private:
    void pcache_init(const q27::PrefixCacheCfg& pc);

    Q27Opts o_;
    std::unique_ptr<q27::Tokenizer> tok_;
    std::unique_ptr<q27::Model> model_;
    std::unique_ptr<q27::DeviceModel> dm_;
    std::unique_ptr<q27::PrefixCache> pcache_;  // declared before eng_: ~Engine joins its writer
    std::unique_ptr<Engine> eng_;
    json tools_;
    std::string decl_, name_;
    unsigned long long seed_ = 0;
};

void Q27Backend::pcache_init(const q27::PrefixCacheCfg& pc) {
    // Must agree with Engine::init's Q27_KV parse (server.cu:986-1000).
    const char* kve = getenv("Q27_KV");
    const int kvk = kve && !strcmp(kve, "fp8")       ? KV_FP8
                    : kve && !strcmp(kve, "turbo3")  ? KV_T3
                    : kve && !strcmp(kve, "turbo3v") ? KV_T3V
                    : kve && !strcmp(kve, "turbo5k") ? KV_T5K
                    : kve && !strcmp(kve, "int8g64") ? KV_I8G64
                                                     : KV_F16;
    std::error_code ec;
    uint64_t bytes = std::filesystem::file_size(o_.model, ec);
    uint64_t compat = q27::pfx_compat_hash(o_.model, bytes, N_LAYER, N_KV, HEAD_DIM, GDN_HEADS, GDN_DIM, GDN_CH,
                                           kvk, q27::PFX_VERSION);
    pcache_->init(pc, compat);
}

}  // namespace

std::unique_ptr<Backend> make_q27_backend(const Q27Opts& o) { return std::make_unique<Q27Backend>(o); }

}  // namespace pk
