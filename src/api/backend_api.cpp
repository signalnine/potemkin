// OpenAI-compatible chat/completions backend with SSE streaming.
#include "backend_api.h"

#ifdef PK_TLS
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include "httplib.h"
#include "json.hpp"

namespace pk {
namespace {

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

std::string call_id(size_t msg, size_t k) { return "call_" + std::to_string(msg) + "_" + std::to_string(k); }

struct PartialCall {
    std::string name, args;
};

class ApiBackend : public Backend {
public:
    explicit ApiBackend(const ApiOpts& o) : o_(o) {
        tools_ = ojson::parse(kToolsJson);
        // Split https://host:port/prefix into the client base and the path prefix.
        std::string u = o.url;
        size_t scheme = u.find("://");
        size_t slash = u.find('/', scheme == std::string::npos ? 0 : scheme + 3);
        base_ = slash == std::string::npos ? u : u.substr(0, slash);
        prefix_ = slash == std::string::npos ? "" : u.substr(slash);
        while (!prefix_.empty() && prefix_.back() == '/') prefix_.pop_back();
        name_ = "api:" + o.model;
    }

    int context_limit() const override { return o_.context; }
    std::string name() const override { return name_; }

    GenResult generate(const std::vector<Message>& msgs, StreamSink& sink,
                       const std::atomic<bool>& cancel) override {
        GenResult res;
        ojson body;
        body["model"] = o_.model;
        body["stream"] = true;
        body["stream_options"] = {{"include_usage", true}};
        body["tools"] = tools_;
        body["messages"] = render(msgs);

        httplib::Client cli(base_);
        cli.set_connection_timeout(std::min(o_.timeout_s, 30), 0);
        cli.set_read_timeout(o_.timeout_s, 0);
#ifdef PK_TLS
        if (!o_.ca_file.empty()) cli.set_ca_cert_path(o_.ca_file);
#endif
        httplib::Headers hdr = {{"Accept", "text/event-stream"}};
        if (!o_.key.empty()) hdr.emplace("Authorization", "Bearer " + o_.key);

        std::string pending, raw;
        std::vector<PartialCall> calls;
        std::string finish;
        auto on_event = [&](const std::string& data) {
            if (data == "[DONE]") return;
            json j = json::parse(data, nullptr, false);
            if (j.is_discarded() || !j.is_object()) return;
            if (j.contains("usage") && j["usage"].is_object()) {
                res.prompt_tokens = j["usage"].value("prompt_tokens", 0);
                res.gen_tokens = j["usage"].value("completion_tokens", 0);
            }
            if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;
            const json& ch = j["choices"][0];
            if (ch.contains("finish_reason") && ch["finish_reason"].is_string()) finish = ch["finish_reason"];
            if (!ch.contains("delta") || !ch["delta"].is_object()) return;
            const json& d = ch["delta"];
            for (const char* rk : {"reasoning_content", "reasoning"}) {
                if (d.contains(rk) && d[rk].is_string()) {
                    const std::string& t = d[rk].get_ref<const std::string&>();
                    if (!t.empty()) { sink.think(t); res.reasoning += t; }
                    break;
                }
            }
            if (d.contains("content") && d["content"].is_string()) {
                const std::string& t = d["content"].get_ref<const std::string&>();
                if (!t.empty()) { sink.text(t); res.text += t; }
            }
            if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
                for (const json& tc : d["tool_calls"]) {
                    size_t i = tc.value("index", 0);
                    if (calls.size() <= i) calls.resize(i + 1);
                    if (tc.contains("function") && tc["function"].is_object()) {
                        const json& f = tc["function"];
                        if (f.contains("name") && f["name"].is_string()) calls[i].name += f["name"].get<std::string>();
                        if (f.contains("arguments") && f["arguments"].is_string())
                            calls[i].args += f["arguments"].get<std::string>();
                    }
                }
            }
        };
        auto receiver = [&](const char* data, size_t n) {
            raw.append(data, std::min<size_t>(n, 4096 - std::min<size_t>(raw.size(), 4096)));
            pending.append(data, n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("data:", 0) == 0) {
                    size_t s = line.find_first_not_of(' ', 5);
                    on_event(s == std::string::npos ? "" : line.substr(s));
                }
            }
            return !cancel.load();
        };
        auto r = cli.Post(prefix_ + "/chat/completions", hdr, body.dump(), "application/json", receiver);

        if (cancel.load()) { res.end = "cancelled"; return res; }
        if (!r) {
            res.end = "error";
            res.text = "api: " + httplib::to_string(r.error()) + " (" + o_.url + ")";
            return res;
        }
        if (r->status != 200) {
            res.end = "error";
            res.text = "api: HTTP " + std::to_string(r->status) + ": " + (raw.empty() ? r->body : raw).substr(0, 400);
            return res;
        }
        for (auto& c : calls) {
            ToolCallRec rec;
            rec.name = c.name;
            ojson a = ojson::parse(c.args.empty() ? "{}" : c.args, nullptr, false);
            if (a.is_discarded() || !a.is_object()) {
                rec.ok = false;
                rec.raw = c.name + " " + c.args;
            } else {
                for (auto it = a.begin(); it != a.end(); ++it)
                    rec.args.emplace_back(it.key(), it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
            }
            res.calls.push_back(std::move(rec));
        }
        res.end = finish == "length" ? "n_max" : "eos";
        return res;
    }

private:
    // Harness messages -> OpenAI messages. Tool results pair with the
    // preceding assistant's calls by position; ids are synthesized.
    ojson render(const std::vector<Message>& msgs) {
        ojson out = ojson::array();
        std::vector<std::string> open_ids;  // ids awaiting their tool message
        size_t next = 0;
        for (size_t mi = 0; mi < msgs.size(); ++mi) {
            const Message& m = msgs[mi];
            if (m.role == "assistant") {
                ojson a;
                a["role"] = "assistant";
                std::string text = m.content;
                ojson tcs = ojson::array();
                open_ids.clear();
                next = 0;
                for (size_t k = 0; k < m.calls.size(); ++k) {
                    const ToolCallRec& c = m.calls[k];
                    if (!c.ok) {
                        text += (text.empty() ? "" : "\n") + c.raw;
                        open_ids.push_back("");
                        continue;
                    }
                    ojson args = ojson::object();
                    for (auto& [key, v] : c.args) args[key] = v;
                    std::string id = call_id(mi, k);
                    tcs.push_back({{"id", id}, {"type", "function"}, {"function", {{"name", c.name}, {"arguments", args.dump()}}}});
                    open_ids.push_back(id);
                }
                a["content"] = text;
                if (!tcs.empty()) a["tool_calls"] = tcs;
                if (!m.reasoning.empty()) a["reasoning_content"] = m.reasoning;
                out.push_back(a);
            } else if (m.role == "tool") {
                std::string id = next < open_ids.size() ? open_ids[next] : "";
                ++next;
                if (id.empty()) out.push_back({{"role", "user"}, {"content", "[tool result] " + m.content}});
                else out.push_back({{"role", "tool"}, {"tool_call_id", id}, {"content", m.content}});
            } else {
                out.push_back({{"role", m.role}, {"content", m.content}});
            }
        }
        return out;
    }

    ApiOpts o_;
    ojson tools_;
    std::string base_, prefix_, name_;
};

}  // namespace

std::unique_ptr<Backend> make_api_backend(const ApiOpts& o) { return std::make_unique<ApiBackend>(o); }

std::string http_fetch(const std::string& url, std::string& err, const std::string& ca_file) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) { err = "not a URL: " + url; return ""; }
    size_t slash = url.find('/', scheme + 3);
    std::string base = slash == std::string::npos ? url : url.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : url.substr(slash);
    httplib::Client cli(base);
    cli.set_follow_location(true);
    cli.set_connection_timeout(15, 0);
    cli.set_read_timeout(60, 0);
#ifdef PK_TLS
    if (!ca_file.empty()) cli.set_ca_cert_path(ca_file);
#else
    (void)ca_file;
#endif
    auto r = cli.Get(path);
    if (!r) { err = httplib::to_string(r.error()); return ""; }
    if (r->status < 200 || r->status >= 300) { err = "HTTP " + std::to_string(r->status); return ""; }
    return r->body;
}

}  // namespace pk
