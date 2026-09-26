// Harness tests with a scripted backend. CPU-only.
#include "../src/harness/harness.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace pk;

static int g_fail = 0, g_run = 0;
static std::string g_cur;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_cur.c_str(), #c); ++g_fail; } } while (0)
#define HAS(s, sub) CHECK(std::string(s).find(sub) != std::string::npos)
#define LACKS(s, sub) CHECK(std::string(s).find(sub) == std::string::npos)

static std::map<std::string, std::function<void()>>& registry() { static std::map<std::string, std::function<void()>> r; return r; }
struct Reg { Reg(const char* n, std::function<void()> f) { registry()[n] = f; } };
#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()

static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static void spit(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream f(p, std::ios::binary); f << s; }

struct FakeBackend : Backend {
    std::deque<GenResult> script;
    std::vector<std::vector<Message>> seen;
    std::function<void()> during;  // runs mid-generate (e.g. to simulate Ctrl-C)
    GenResult generate(const std::vector<Message>& msgs, StreamSink& sink, const std::atomic<bool>& cancel) override {
        seen.push_back(msgs);
        if (during) during();
        GenResult r;
        if (!script.empty()) { r = script.front(); script.pop_front(); } else { r.text = "(no script)"; r.end = "eos"; }
        if (cancel) { r.end = "cancelled"; r.calls.clear(); }
        if (!r.reasoning.empty()) sink.think(r.reasoning);
        sink.text(r.text);
        return r;
    }
    int context_limit() const override { return 1000; }
    std::string name() const override { return "fake"; }
};

struct CaptureConsole : Console {
    std::string all;
    void think(const std::string& s) override { all += "[think]" + s; }
    void text(const std::string& s) override { all += s; }
    void say(const std::string& s) override { all += "[say]" + s + "\n"; }
    void note(const std::string& s) override { all += "[note]" + s + "\n"; }
};

static GenResult text(const std::string& t) { GenResult r; r.text = t; r.end = "eos"; return r; }
static GenResult calls(std::vector<ToolCallRec> cs, const std::string& t = "") { GenResult r; r.text = t; r.calls = std::move(cs); r.end = "eos"; return r; }
static ToolCallRec call(const std::string& n, std::vector<std::pair<std::string, std::string>> a) { ToolCallRec c; c.name = n; c.args = std::move(a); return c; }

struct Env {
    fs::path root; Config hc; HarnessConfig cfg;
    Env() {
        char t[] = "/tmp/pk_harness_XXXXXX"; root = mkdtemp(t);
        for (auto d : {"generated/bin", "state", "store", "data", "intent"}) fs::create_directories(root / d);
        hc.root = root.string(); cfg.system_prompt = "SYS";
    }
    ~Env() { std::error_code ec; fs::remove_all(root, ec); }
};

// ---- turns ----
TEST(plain_turn) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {text("hello user")};
    H.handle_line("hi");
    CHECK(b.seen.size() == 1);
    CHECK(b.seen[0].size() == 2); CHECK(b.seen[0][0].role == "system"); CHECK(b.seen[0][0].content == "SYS");
    CHECK(b.seen[0][1].role == "user"); CHECK(b.seen[0][1].content == "hi");
    HAS(c.all, "hello user");
    HAS(slurp(e.root / "state/transcript.jsonl"), "hello user");
    HAS(slurp(e.root / "intent/log"), "hi"); }
TEST(intent_one_line_per_turn) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {text("a"), text("b")};
    H.handle_line("first"); H.handle_line("second\nline");
    auto log = slurp(e.root / "intent/log");
    CHECK(std::count(log.begin(), log.end(), '\n') == 2); HAS(log, "turn=1"); HAS(log, "turn=2"); }
TEST(empty_line_ignored) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    H.handle_line("   "); CHECK(b.seen.empty()); }
TEST(tool_loop) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {calls({call("write", {{"path", "/data/x"}, {"content", "hey"}})}), text("done")};
    H.handle_line("make x");
    CHECK(slurp(e.root / "data/x") == "hey");
    CHECK(b.seen.size() == 2);
    auto& m = b.seen[1];
    CHECK(m.size() == 4); CHECK(m[2].role == "assistant"); CHECK(m[2].calls.size() == 1);
    CHECK(m[3].role == "tool"); HAS(m[3].content, "wrote 3 bytes");
    HAS(c.all, "done"); }
TEST(multiple_calls_in_order) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {calls({call("write", {{"path", "/data/a"}, {"content", "1"}}), call("read", {{"path", "/data/a"}})}), text("ok")};
    H.handle_line("go");
    auto& m = b.seen[1];
    CHECK(m.size() == 5); CHECK(m[3].role == "tool"); CHECK(m[4].role == "tool");
    HAS(m[3].content, "wrote 1 bytes"); CHECK(m[4].content == "1"); }
TEST(failed_parse_goes_back_to_model) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    ToolCallRec bad; bad.ok = false; bad.raw = "<function=write><parameter=pa";
    b.script = {calls({bad}), text("sorry")};
    H.handle_line("go");
    auto& m = b.seen[1]; CHECK(m.back().role == "tool"); HAS(m.back().content, "error:"); HAS(m.back().content, "could not parse"); }
TEST(unknown_tool_goes_back_to_model) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {calls({call("bash", {{"command", "ls"}})}), text("fine")};
    H.handle_line("go"); HAS(b.seen[1].back().content, "error: unknown tool bash"); }
TEST(tool_activity_shown) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {calls({call("write", {{"path", "/data/a"}, {"content", "1"}})}), text("ok")};
    H.handle_line("go"); HAS(c.all, "[note]"); HAS(c.all, "write /data/a"); }

// ---- snapshots ----
static int count_snaps(const fs::path& root) { int n = 0; std::error_code ec;
    for (auto& d : fs::directory_iterator(root / "snapshots", ec)) { (void)d; ++n; } return n; }
TEST(read_only_turn_no_snapshot) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    spit(e.root / "data/a", "x");
    b.script = {calls({call("read", {{"path", "/data/a"}})}), text("ok")};
    H.handle_line("look"); CHECK(count_snaps(e.root) == 0); }
TEST(one_snapshot_per_mutating_turn) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {calls({call("write", {{"path", "/generated/a"}, {"content", "1"}}), call("write", {{"path", "/generated/b"}, {"content", "2"}})}),
                calls({call("write", {{"path", "/generated/c"}, {"content", "3"}})}), text("ok")};
    H.handle_line("go"); CHECK(count_snaps(e.root) == 1);
    CHECK(!fs::exists(e.root / "snapshots/1/generated/a")); }
TEST(undo_restores_files_and_conversation) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {text("first reply"), calls({call("write", {{"path", "/generated/a"}, {"content", "1"}})}), text("wrote a"), text("after")};
    H.handle_line("one"); H.handle_line("two");
    CHECK(fs::exists(e.root / "generated/a"));
    H.handle_line("/undo");
    CHECK(!fs::exists(e.root / "generated/a"));
    H.handle_line("three");
    auto& m = b.seen.back();
    bool saw_two = false; for (auto& x : m) if (x.content == "two") saw_two = true;
    CHECK(!saw_two); CHECK(m.size() == 4); CHECK(m[1].content == "one"); CHECK(m[3].content == "three"); }
TEST(undo_twice_walks_back) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {calls({call("write", {{"path", "/generated/a"}, {"content", "1"}})}), text("a"),
                calls({call("write", {{"path", "/generated/b"}, {"content", "2"}})}), text("b")};
    H.handle_line("one"); H.handle_line("two");
    H.handle_line("/undo"); CHECK(fs::exists(e.root / "generated/a")); CHECK(!fs::exists(e.root / "generated/b"));
    H.handle_line("/undo"); CHECK(!fs::exists(e.root / "generated/a")); }
TEST(undo_nothing) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    H.handle_line("/undo"); HAS(c.all, "nothing to undo"); CHECK(b.seen.empty()); }
TEST(model_rollback_keeps_memory) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {calls({call("write", {{"path", "/generated/a"}, {"content", "1"}})}), text("a"),
                calls({call("snapshot", {{"rollback", "1"}})}), text("rolled")};
    H.handle_line("one"); H.handle_line("roll it back");
    CHECK(!fs::exists(e.root / "generated/a"));
    HAS(slurp(e.root / "state/transcript.jsonl"), "roll it back"); CHECK(H.messages().size() >= 7); }

// ---- interrupts ----
TEST(cancel_stops_turn) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {calls({call("write", {{"path", "/data/a"}, {"content", "1"}})}), text("never")};
    b.during = [&] { H.cancel = true; };
    H.handle_line("go");
    CHECK(b.seen.size() == 1); CHECK(!fs::exists(e.root / "data/a")); HAS(c.all, "interrupted");
    b.during = nullptr; b.script = {text("next")};
    H.handle_line("again"); CHECK(b.seen.size() == 2); HAS(c.all, "next"); }

// ---- slash commands ----
TEST(slash_never_reaches_model) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    H.handle_line("/help"); H.handle_line("/nonsense"); H.handle_line("/procs");
    CHECK(b.seen.empty()); HAS(c.all, "unknown command /nonsense"); }
TEST(help_lists_tools_and_skills) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    spit(e.root / "generated/skills/proxy.md", "Set up the reverse proxy\nsteps...\n");
    H.handle_line("/help");
    for (auto t : {"read", "write", "stat", "spawn", "wait", "compile", "snapshot", "fetch"}) HAS(c.all, t);
    HAS(c.all, "/undo"); HAS(c.all, "/reboot"); HAS(c.all, "proxy"); HAS(c.all, "Set up the reverse proxy"); LACKS(c.all, "steps..."); }
TEST(skill_injects_body_and_args) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    spit(e.root / "generated/skills/proxy.md", "Set up the reverse proxy\nListen on $1.\n");
    b.script = {text("ok")};
    H.handle_line("/skill proxy 8080 8443");
    CHECK(b.seen.size() == 1); auto& u = b.seen[0].back(); CHECK(u.role == "user");
    HAS(u.content, "Listen on $1."); HAS(u.content, "8080 8443"); HAS(slurp(e.root / "intent/log"), "/skill proxy"); }
TEST(skill_missing) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    H.handle_line("/skill nope"); CHECK(b.seen.empty()); HAS(c.all, "no skill"); }
TEST(procs_and_log) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    spit(e.root / "state/procs", "123 background running turn=1 srv\n"); spit(e.root / "state/log/123", "line1\nline2\n");
    H.handle_line("/procs"); HAS(c.all, "123 background running");
    H.handle_line("/log 123"); HAS(c.all, "line2"); }
TEST(kill_command) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    fs::create_directories(e.root / "bin"); fs::create_symlink("/bin/sleep", e.root / "bin/sleep");
    b.script = {calls({call("spawn", {{"exe", "/bin/sleep"}, {"argv", "30"}, {"mode", "background"}})}), text("started")};
    H.handle_line("start");
    auto procs = slurp(e.root / "state/procs"); std::string pid = procs.substr(0, procs.find(' '));
    H.handle_line("/kill " + pid);
    HAS(c.all, "signal=15"); CHECK(b.seen.size() == 2); }
TEST(reboot_snapshots_and_returns) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    CHECK(H.handle_line("/reboot") == Action::Reboot); CHECK(count_snaps(e.root) == 1); CHECK(b.seen.empty()); }

// ---- persistence ----
TEST(transcript_restored_on_boot) { Env e;
    { Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
      b.script = {calls({call("write", {{"path", "/data/a"}, {"content", "multi\nline <x>"}})}, "writing"), text("first answer")};
      H.handle_line("remember me"); }
    Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg); H.boot();
    b.script = {text("second")}; H.handle_line("again");
    auto& m = b.seen[0];
    CHECK(m.size() == 6); CHECK(m[1].content == "remember me");
    CHECK(m[2].calls.size() == 1); CHECK(m[2].calls[0].args[1].second == "multi\nline <x>");
    CHECK(m[4].content == "first answer"); CHECK(m[5].content == "again"); }
TEST(transcript_roundtrip) {
    Message m; m.role = "assistant"; m.content = "a\"b\\c\n"; m.reasoning = "think\t\x01";
    ToolCallRec tc = call("compile", {{"lang", "c"}, {"source", "int main(){return 0;}\n"}}); m.calls = {tc};
    ToolCallRec bad; bad.ok = false; bad.raw = "<junk"; m.calls.push_back(bad);
    Message o; CHECK(message_from_json(message_to_json(m), o));
    CHECK(o.role == m.role); CHECK(o.content == m.content); CHECK(o.reasoning == m.reasoning);
    CHECK(o.calls.size() == 2); CHECK(o.calls[0].args == tc.args); CHECK(!o.calls[1].ok); CHECK(o.calls[1].raw == "<junk"); }
TEST(boot_banner) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    H.boot(); HAS(c.all, "potemkin"); HAS(c.all, "there is nothing here yet"); }
TEST(boot_banner_returning) { Env e;
    { Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg); b.script = {text("x")}; H.handle_line("hi"); }
    Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg); H.boot(); LACKS(c.all, "there is nothing here yet"); }

// ---- compaction ----
TEST(compaction_when_near_limit) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    GenResult big = text("long answer"); big.prompt_tokens = 900;  // limit 1000, compact_at .75
    GenResult summary = text("SUMMARY: user said hi");
    b.script = {big, summary, text("fresh")};
    H.handle_line("hi");
    CHECK(b.seen.size() == 2);  // the answer, then the summarization call
    HAS(b.seen[1].back().content, "ummar");
    HAS(slurp(e.root / "state/summaries"), "SUMMARY: user said hi");
    H.handle_line("next");
    auto& m = b.seen.back();
    CHECK(m[0].content == "SYS");  // system prompt unchanged: the prefix-cache cut must not move
    HAS(m[1].content, "SUMMARY: user said hi"); CHECK(m.back().content == "next"); CHECK(m.size() == 3); }

TEST(compaction_fallback_drops_old_turns) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {text("a1"), text("a2"), text("a3")};
    H.handle_line("q1"); H.handle_line("q2");
    GenResult big = text("a3"); big.prompt_tokens = 950;
    GenResult empty; empty.end = "n_max";  // summarizer ran out of room
    b.script = {big, empty, text("a4")};
    H.handle_line("q3");
    H.handle_line("q4");
    auto& m = b.seen.back();
    CHECK(m[0].content == "SYS");
    bool has_q1 = false; for (auto& x : m) if (x.content == "q1") has_q1 = true;
    CHECK(!has_q1);
    HAS(m[1].content, "dropped");
    CHECK(m[m.size() - 2].content == "a3"); CHECK(m.back().content == "q4");
    CHECK(m[2].role == "user"); }
TEST(reply_cut_off_is_visible) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    GenResult r = text("half a thou"); r.end = "n_max";
    b.script = {r};
    H.handle_line("go"); HAS(c.all, "cut off"); }

TEST(compaction_appends_ledger_ground_truth) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    fs::create_directories(e.root / "store/sha256:aaa");
    fs::create_symlink("/store/sha256:aaa/bin", e.root / "generated/bin/ls");
    GenResult big = text("x"); big.prompt_tokens = 900;
    b.script = {big, text("SUMMARY: I built ls and sh (sha256:bbb)"), text("ok")};
    H.handle_line("hi");
    H.handle_line("next");
    auto& m = b.seen.back();
    HAS(m[1].content, "SUMMARY: I built ls and sh");
    HAS(m[1].content, "ls -> /store/sha256:aaa/bin");
    HAS(m[1].content, "[harness] /generated/bin right now"); }

TEST(backend_error_mid_turn_keeps_pairs) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    GenResult boom; boom.end = "error"; boom.text = "api: connection reset";
    b.script = {calls({call("write", {{"path", "/data/a"}, {"content", "1"}}), call("read", {{"path", "/data/a"}})}), boom};
    H.handle_line("go");
    auto& m = H.messages();
    CHECK(m.size() == 5);  // system, user, assistant(2 calls), tool, tool
    CHECK(m[2].calls.size() == 2); CHECK(m[3].role == "tool"); CHECK(m[4].role == "tool");
    HAS(c.all, "connection reset"); }
TEST(backend_error_first_round_drops_turn) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    GenResult boom; boom.end = "error"; boom.text = "api: down";
    b.script = {boom};
    H.handle_line("go");
    CHECK(H.messages().size() == 1); }

TEST(undo_survives_restart) { Env e;
    { Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
      b.script = {calls({call("write", {{"path", "/generated/a"}, {"content", "1"}})}), text("ok")};
      H.handle_line("make a"); }
    Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg); H.boot();
    H.handle_line("/undo");
    LACKS(c.all, "nothing to undo"); CHECK(!fs::exists(e.root / "generated/a")); }

TEST(transcript_saved_every_round) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {calls({call("write", {{"path", "/data/a"}, {"content", "1"}})}), text("done")};
    std::string mid;
    int n = 0;
    b.during = [&] { if (++n == 2) mid = slurp(e.root / "state/transcript.jsonl"); };
    H.handle_line("go");
    HAS(mid, "wrote 1 bytes"); HAS(mid, "\"go\""); }

TEST(compaction_mid_turn_between_rounds) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    GenResult r1 = calls({call("write", {{"path", "/data/a"}, {"content", "1"}})}); r1.prompt_tokens = 900;  // limit 1000
    b.script = {r1, text("SUMMARY: wrote /data/a, next write /data/b"),
                calls({call("write", {{"path", "/data/b"}, {"content", "2"}})}), text("done")};
    H.handle_line("write a then b");
    CHECK(b.seen.size() == 4);
    auto& after = b.seen[2];  // first request after compaction, same turn
    CHECK(after[0].content == "SYS");
    HAS(after[1].content, "SUMMARY: wrote /data/a");
    HAS(after.back().content, "Continue where you left off");
    CHECK(fs::exists(e.root / "data/b")); HAS(c.all, "done"); }

TEST(too_long_prompt_drops_history_and_retries) { Env e; Host h(e.hc); FakeBackend b; CaptureConsole c; Harness H(b, h, c, e.cfg);
    b.script = {text("a1"), text("a2"), text("a3")};
    H.handle_line("q1"); H.handle_line("q2"); H.handle_line("q3");
    GenResult tooLong; tooLong.end = "error"; tooLong.text = "api: HTTP 400: context_length_exceeded";
    b.script = {tooLong, text("recovered")};
    H.handle_line("q4");
    HAS(c.all, "recovered");
    auto& m = b.seen.back();
    bool has_q1 = false; for (auto& x : m) if (x.content == "q1") has_q1 = true;
    CHECK(!has_q1); CHECK(m.back().content == "q4"); }

// ---- frozen blocks ----
TEST(tools_json_has_eight_in_order) {
    std::string t = kToolsJson; size_t pos = 0; std::vector<std::string> names;
    while ((pos = t.find("\"name\": \"", pos)) != std::string::npos) { pos += 9; names.push_back(t.substr(pos, t.find('"', pos) - pos)); }
    CHECK((names == std::vector<std::string>{"read", "write", "stat", "spawn", "wait", "compile", "snapshot", "fetch"})); }
TEST(system_prompt_short) { CHECK(std::string(kSystemPrompt).size() < 900); HAS(kSystemPrompt, "no userland"); }

int main(int argc, char** argv) {
    for (auto& [n, f] : registry()) {
        if (argc > 1 && n.find(argv[1]) == std::string::npos) continue;
        g_cur = n; int before = g_fail; ++g_run; f();
        std::printf("%s %s\n", g_fail == before ? "ok  " : "FAIL", n.c_str());
    }
    std::printf("%d tests, %d failed checks\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
