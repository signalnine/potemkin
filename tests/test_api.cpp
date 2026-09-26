// llm=api backend against an in-process fake OpenAI-compatible server.
#include "../src/api/backend_api.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <thread>

#include "httplib.h"
#include "json.hpp"

using namespace pk;
using json = nlohmann::json;

static int g_fail = 0, g_run = 0;
static std::string g_cur;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_cur.c_str(), #c); ++g_fail; } } while (0)
#define HAS(s, sub) CHECK(std::string(s).find(sub) != std::string::npos)
static std::map<std::string, std::function<void()>>& registry() { static std::map<std::string, std::function<void()>> r; return r; }
struct Reg { Reg(const char* n, std::function<void()> f) { registry()[n] = f; } };
#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()

struct Sink : StreamSink {
    std::string th, tx;
    void think(const std::string& s) override { th += s; }
    void text(const std::string& s) override { tx += s; }
};

// Serves one canned SSE stream (or status) per request and records bodies.
struct Fake {
    httplib::Server srv;
    std::thread th;
    int port = 0;
    std::vector<json> bodies;
    std::vector<std::string> auth;
    std::vector<std::vector<std::string>> streams;  // chunks, each a JSON object (sent as data: lines)
    int status = 200;
    int delay_ms = 0;
    Fake() {
        srv.Post("/v1/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
            bodies.push_back(json::parse(req.body));
            auth.push_back(req.get_header_value("Authorization"));
            if (status != 200) { res.status = status; res.set_content("{\"error\":{\"message\":\"boom\"}}", "application/json"); return; }
            std::vector<std::string> chunks = streams.empty() ? std::vector<std::string>{} : streams.front();
            if (!streams.empty()) streams.erase(streams.begin());
            int d = delay_ms;
            res.set_chunked_content_provider("text/event-stream", [chunks, d](size_t, httplib::DataSink& sink) {
                for (auto& c : chunks) {
                    std::string line = "data: " + c + "\n\n";
                    if (!sink.write(line.data(), line.size())) return false;
                    if (d) std::this_thread::sleep_for(std::chrono::milliseconds(d));
                }
                std::string done = "data: [DONE]\n\n";
                sink.write(done.data(), done.size());
                sink.done();
                return true;
            });
        });
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
        while (!srv.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ~Fake() { srv.stop(); th.join(); }
    ApiOpts opts() { ApiOpts o; o.url = "http://127.0.0.1:" + std::to_string(port) + "/v1"; o.key = "sk-test"; o.model = "m1"; return o; }
};

static std::string delta(json d, const char* finish = nullptr) {
    json c = {{"choices", json::array({{{"index", 0}, {"delta", d}}})}};
    if (finish) c["choices"][0]["finish_reason"] = finish;
    return c.dump();
}

static std::vector<Message> convo() { return {{"system", "SYS", "", {}}, {"user", "hi", "", {}}}; }

TEST(request_shape) { Fake f; f.streams = {{delta({{"content", "ok"}}, "stop")}};
    auto b = make_api_backend(f.opts()); Sink s; std::atomic<bool> no{false};
    b->generate(convo(), s, no);
    CHECK(f.bodies.size() == 1); auto& j = f.bodies[0];
    CHECK(j["model"] == "m1"); CHECK(j["stream"] == true);
    CHECK(j["messages"][0]["role"] == "system"); CHECK(j["messages"][0]["content"] == "SYS");
    CHECK(j["messages"][1]["role"] == "user"); CHECK(j["messages"][1]["content"] == "hi");
    CHECK(j["tools"].size() == 8); CHECK(j["tools"][5]["function"]["name"] == "compile");
    CHECK(f.auth[0] == "Bearer sk-test"); }
TEST(streams_text_and_reasoning) { Fake f;
    f.streams = {{delta({{"reasoning_content", "hmm "}}), delta({{"reasoning_content", "ok"}}), delta({{"content", "hel"}}), delta({{"content", "lo"}}, "stop")}};
    auto b = make_api_backend(f.opts()); Sink s; std::atomic<bool> no{false};
    auto r = b->generate(convo(), s, no);
    CHECK(s.tx == "hello"); CHECK(s.th == "hmm ok"); CHECK(r.text == "hello"); CHECK(r.reasoning == "hmm ok"); CHECK(r.end == "eos"); }
TEST(assembles_tool_calls) { Fake f;
    json tc0 = {{"index", 0}, {"id", "abc"}, {"type", "function"}, {"function", {{"name", "write"}, {"arguments", "{\"path\": \"/data/x\", "}}}};
    json tc0b = {{"index", 0}, {"function", {{"arguments", "\"content\": \"line1\\nline2\"}"}}}};
    json tc1 = {{"index", 1}, {"id", "def"}, {"type", "function"}, {"function", {{"name", "read"}, {"arguments", "{\"path\": \"/data/x\", \"offset\": 3}"}}}};
    f.streams = {{delta({{"content", "doing it"}}), delta({{"tool_calls", {tc0}}}), delta({{"tool_calls", {tc0b}}}), delta({{"tool_calls", {tc1}}}, "tool_calls")}};
    auto b = make_api_backend(f.opts()); Sink s; std::atomic<bool> no{false};
    auto r = b->generate(convo(), s, no);
    CHECK(r.calls.size() == 2);
    CHECK(r.calls[0].name == "write"); CHECK(r.calls[0].ok);
    CHECK(r.calls[0].args.size() == 2); CHECK(r.calls[0].args[0].first == "path"); CHECK(r.calls[0].args[1].second == "line1\nline2");
    CHECK(r.calls[1].args[1].first == "offset"); CHECK(r.calls[1].args[1].second == "3"); }
TEST(bad_arguments_not_ok) { Fake f;
    json tc = {{"index", 0}, {"id", "x"}, {"type", "function"}, {"function", {{"name", "write"}, {"arguments", "{\"path\": "}}}};
    f.streams = {{delta({{"tool_calls", {tc}}}, "tool_calls")}};
    auto b = make_api_backend(f.opts()); Sink s; std::atomic<bool> no{false};
    auto r = b->generate(convo(), s, no);
    CHECK(r.calls.size() == 1); CHECK(!r.calls[0].ok); HAS(r.calls[0].raw, "{\"path\": "); }
TEST(history_ids_match) { Fake f; f.streams = {{delta({{"content", "k"}}, "stop")}};
    auto b = make_api_backend(f.opts()); Sink s; std::atomic<bool> no{false};
    auto m = convo();
    Message a{"assistant", "sure", "", {}};
    ToolCallRec c1; c1.name = "write"; c1.args = {{"path", "/a"}, {"content", "1"}};
    ToolCallRec c2; c2.name = "read"; c2.args = {{"path", "/a"}};
    a.calls = {c1, c2};
    m.push_back(a); m.push_back({"tool", "wrote 1 bytes", "", {}}); m.push_back({"tool", "1", "", {}});
    b->generate(m, s, no);
    auto& ms = f.bodies[0]["messages"];
    CHECK(ms.size() == 5);
    auto& tcs = ms[2]["tool_calls"]; CHECK(tcs.size() == 2);
    CHECK(tcs[0]["function"]["name"] == "write");
    CHECK(json::parse(tcs[0]["function"]["arguments"].get<std::string>())["content"] == "1");
    CHECK(ms[3]["role"] == "tool"); CHECK(ms[3]["tool_call_id"] == tcs[0]["id"]);
    CHECK(ms[4]["tool_call_id"] == tcs[1]["id"]); CHECK(ms[4]["content"] == "1"); }
TEST(failed_call_history_is_text) { Fake f; f.streams = {{delta({{"content", "k"}}, "stop")}};
    auto b = make_api_backend(f.opts()); Sink s; std::atomic<bool> no{false};
    auto m = convo();
    Message a{"assistant", "", "", {}}; ToolCallRec bad; bad.ok = false; bad.raw = "<junk>"; a.calls = {bad};
    m.push_back(a); m.push_back({"tool", "error: could not parse", "", {}});
    b->generate(m, s, no);
    auto& ms = f.bodies[0]["messages"];
    CHECK(!ms[2].contains("tool_calls")); HAS(ms[2]["content"].get<std::string>(), "<junk>");
    CHECK(ms[3]["role"] == "user"); HAS(ms[3]["content"].get<std::string>(), "could not parse"); }
TEST(http_error) { Fake f; f.status = 500;
    auto b = make_api_backend(f.opts()); Sink s; std::atomic<bool> no{false};
    auto r = b->generate(convo(), s, no); CHECK(r.end == "error"); HAS(r.text, "500"); }
TEST(unreachable) { ApiOpts o; o.url = "http://127.0.0.1:1/v1"; o.timeout_s = 2;
    auto b = make_api_backend(o); Sink s; std::atomic<bool> no{false};
    auto r = b->generate(convo(), s, no); CHECK(r.end == "error"); }
TEST(cancel_mid_stream) { Fake f; f.delay_ms = 100;
    std::vector<std::string> many; for (int i = 0; i < 50; ++i) many.push_back(delta({{"content", "x"}}));
    f.streams = {many};
    auto b = make_api_backend(f.opts()); std::atomic<bool> cancel{false};
    struct CSink : Sink { std::atomic<bool>* c; void text(const std::string& t) override { tx += t; if (tx.size() >= 3) *c = true; } } s; s.c = &cancel;
    auto t0 = std::chrono::steady_clock::now();
    auto r = b->generate(convo(), s, cancel);
    CHECK(r.end == "cancelled"); CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)); }
TEST(reasoning_sent_back_only_when_present) { Fake f; f.streams = {{delta({{"content", "k"}}, "stop")}};
    auto b = make_api_backend(f.opts()); Sink s; std::atomic<bool> no{false};
    auto m = convo(); m.push_back({"assistant", "a", "thought", {}}); m.push_back({"assistant", "b", "", {}}); m.push_back({"user", "q", "", {}});
    b->generate(m, s, no);
    auto& ms = f.bodies[0]["messages"];
    CHECK(ms[2]["reasoning_content"] == "thought"); CHECK(!ms[3].contains("reasoning_content")); }

int main(int argc, char** argv) {
    for (auto& [n, fn] : registry()) {
        if (argc > 1 && n.find(argv[1]) == std::string::npos) continue;
        g_cur = n; int before = g_fail; ++g_run; fn();
        std::printf("%s %s\n", g_fail == before ? "ok  " : "FAIL", n.c_str());
    }
    std::printf("%d tests, %d failed checks\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
