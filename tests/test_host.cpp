// Tests for src/host (CONTRACT.md). CPU-only; uses the host's userland for
// spawn targets and build/sysroot for tcc + musl.
#include "../src/host/host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <chrono>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using pk::Args;
using pk::Config;
using pk::Host;

static int g_fail = 0, g_run = 0;
static std::string g_cur;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_cur.c_str(), #c); ++g_fail; } } while (0)
#define HAS(s, sub) CHECK(std::string(s).find(sub) != std::string::npos)
#define LACKS(s, sub) CHECK(std::string(s).find(sub) == std::string::npos)

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
static void spit(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary); f << s;
}

struct Env {
    fs::path root;
    Config cfg;
    Env() {
        char tmpl[] = "/tmp/pk_host_XXXXXX";
        root = mkdtemp(tmpl);
        cfg.root = root.string();
        fs::create_directories(root / "generated/bin");
        fs::create_directories(root / "state/log");
        fs::create_directories(root / "store");
        const char* sr = std::getenv("PK_SYSROOT");
        std::string s = sr ? sr : "build/sysroot";
        s = fs::absolute(s).string();
        std::string m = s + "/usr/lib/x86_64-linux-musl", t = s + "/usr/lib/x86_64-linux-gnu/tcc";
        cfg.cc = {s + "/usr/bin/tcc", "-nostdinc", "-nostdlib", "-static",
                  "-I" + s + "/usr/include/x86_64-linux-musl", "-I" + t + "/include",
                  m + "/crt1.o", m + "/crti.o", "@SRC@", m + "/libc.a", t + "/libtcc1.a", m + "/crtn.o"};
        cfg.model_name = "test-model";
    }
    ~Env() { std::error_code ec; fs::remove_all(root, ec); }
};

static std::map<std::string, std::function<void()>>& registry() {
    static std::map<std::string, std::function<void()>> r; return r;
}
struct Reg { Reg(const char* n, std::function<void()> f) { registry()[n] = f; } };
static std::string pid_of(const std::string& b) { return std::to_string(std::atoi(b.substr(b.find("pid=") + 4).c_str())); }
#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()

// ---- dispatch ----
TEST(unknown_tool) { Env e; Host h(e.cfg); HAS(h.call("bash", {}).body, "error: unknown tool bash"); }
TEST(missing_arg) { Env e; Host h(e.cfg); HAS(h.call("read", {}).body, "error: read: missing path"); }
TEST(bad_int) { Env e; Host h(e.cfg); spit(e.root / "a", "x");
    HAS(h.call("read", {{"path", "/a"}, {"offset", "abc"}}).body, "error:"); }
TEST(relative_path) { Env e; Host h(e.cfg); HAS(h.call("read", {{"path", "a"}}).body, "error:"); }
TEST(tool_names_frozen) { Env e; Host h(e.cfg);
    auto n = h.tool_names();
    CHECK((n == std::vector<std::string>{"read", "write", "stat", "spawn", "wait", "compile", "snapshot", "fetch"})); }

// ---- read ----
TEST(read_text) { Env e; Host h(e.cfg); spit(e.root / "d/f.txt", "hello\nworld\n");
    auto r = h.call("read", {{"path", "/d/f.txt"}}); CHECK(r.body == "hello\nworld\n"); CHECK(!r.truncated); }
TEST(read_range) { Env e; Host h(e.cfg); spit(e.root / "f", "0123456789");
    CHECK(h.call("read", {{"path", "/f"}, {"offset", "3"}, {"len", "4"}}).body == "3456"); }
TEST(read_truncates) { Env e; e.cfg.max_result_bytes = 100; Host h(e.cfg); spit(e.root / "f", std::string(1000, 'a'));
    auto r = h.call("read", {{"path", "/f"}}); CHECK(r.truncated);
    HAS(r.body, "[truncated: bytes 0-100 of 1000; read offset=100 to continue]");
    CHECK(r.body.size() < 200); }
TEST(read_binary_hexdump) { Env e; Host h(e.cfg); spit(e.root / "b", std::string("\x7f" "ELF\x02\x01\x01\x00\xff\xfe", 10));
    auto r = h.call("read", {{"path", "/b"}}); HAS(r.body, "[binary: hexdump"); HAS(r.body, "7f 45 4c 46"); }
TEST(read_missing) { Env e; Host h(e.cfg); HAS(h.call("read", {{"path", "/nope"}}).body, "error:"); }
TEST(proc_sys_dev_pass_through_root) { Env e; Host h(e.cfg);
    HAS(h.call("read", {{"path", "/proc/self/status"}}).body, "Name:");
    HAS(h.call("stat", {{"path", "/sys"}, {"list", "1"}}).body, "class/");
    HAS(h.call("stat", {{"path", "/dev/null"}}).body, "chardev");
    CHECK(h.real("/procfoo") == e.root.string() + "/procfoo"); }
TEST(read_proc) { Config c; c.root = ""; Host h(c);
    auto r = h.call("read", {{"path", "/proc/self/status"}}); HAS(r.body, "Name:"); }

// ---- write ----
TEST(write_creates_parents) { Env e; Host h(e.cfg);
    auto r = h.call("write", {{"path", "/generated/src/x/y.c"}, {"content", "int main(){}"}});
    LACKS(r.body, "error"); CHECK(slurp(e.root / "generated/src/x/y.c") == "int main(){}"); }
TEST(write_exact) { Env e; Host h(e.cfg); std::string c = "a < b && c > d\n\n\ttab\n";
    auto r = h.call("write", {{"path", "/f"}, {"content", c}}); CHECK(slurp(e.root / "f") == c);
    HAS(r.body, std::to_string(c.size()) + " bytes"); }

// ---- stat ----
TEST(stat_file) { Env e; Host h(e.cfg); spit(e.root / "f", "12345"); chmod((e.root / "f").c_str(), 0640);
    auto b = h.call("stat", {{"path", "/f"}}).body; HAS(b, "file"); HAS(b, "size=5"); HAS(b, "mode=0640"); HAS(b, "mtime="); }
TEST(stat_list) { Env e; Host h(e.cfg); spit(e.root / "d/b", ""); spit(e.root / "d/a", ""); fs::create_directories(e.root / "d/sub");
    fs::create_symlink("a", e.root / "d/ln");
    auto b = h.call("stat", {{"path", "/d"}, {"list", "1"}}).body; CHECK(b == "a\nb\nln -> a\nsub/\n"); }
TEST(stat_missing) { Env e; Host h(e.cfg); HAS(h.call("stat", {{"path", "/x"}}).body, "error:"); }

// ---- spawn ----
TEST(spawn_argv_split) {
    CHECK((pk::split_argv("a b  'c d' \"e f\" g") == std::vector<std::string>{"a", "b", "c d", "e f", "g"}));
    CHECK(pk::split_argv("").empty()); }
TEST(spawn_capture) { Config c; c.root = ""; Host h(c);
    auto b = h.call("spawn", {{"exe", "/bin/echo"}, {"argv", "hi there"}}).body;
    HAS(b, "exit=0"); HAS(b, "hi there"); }
TEST(spawn_stdin) { Config c; c.root = ""; Host h(c);
    HAS(h.call("spawn", {{"exe", "/bin/cat"}, {"stdin", "piped!"}}).body, "piped!"); }
TEST(spawn_timeout) { Config c; c.root = ""; Host h(c);
    auto b = h.call("spawn", {{"exe", "/bin/sleep"}, {"argv", "10"}, {"timeout_s", "1"}}).body; HAS(b, "timeout"); }
TEST(spawn_missing_exe) { Env e; Host h(e.cfg);
    HAS(h.call("spawn", {{"exe", "/generated/bin/nothing"}}).body, "error:"); }
TEST(spawn_background) { Env e; Host h(e.cfg);
    fs::create_directories(e.root / "bin"); fs::create_symlink("/bin/echo", e.root / "bin/echo");
    auto b = h.call("spawn", {{"exe", "/bin/echo"}, {"argv", "bg-out"}, {"mode", "background"}}).body;
    HAS(b, "pid="); pid_t pid = std::atoi(b.substr(b.find("pid=") + 4).c_str()); CHECK(pid > 0);
    usleep(200000); HAS(slurp(e.root / "state/log" / std::to_string(pid)), "bg-out"); }
TEST(procs_table) { Env e; Host h(e.cfg); fs::create_directories(e.root / "bin"); fs::create_symlink("/bin/sleep", e.root / "bin/sleep");
    h.set_turn(7);
    auto b = h.call("spawn", {{"exe", "/bin/sleep"}, {"argv", "5"}, {"mode", "background"}}).body;
    auto t = slurp(e.root / "state/procs"); HAS(t, "background"); HAS(t, "running"); HAS(t, "turn=7"); HAS(t, "sleep 5");
    pid_t pid = std::atoi(b.substr(b.find("pid=") + 4).c_str());
    h.call("wait", {{"pid", std::to_string(pid)}, {"signal", "KILL"}});
    HAS(slurp(e.root / "state/procs"), "exited"); }
TEST(spawn_cgroup_files) { Env e; fs::path cg = e.root / "cg"; fs::create_directories(cg); e.cfg.cgroup_root = cg.string();
    Config c = e.cfg; c.root = ""; Host h(c);
    auto b = h.call("spawn", {{"exe", "/bin/true"}, {"mem_mb", "64"}});
    HAS(b.body, "exit=0");
    bool found = false;
    for (auto& d : fs::directory_iterator(cg / "potemkin")) {
        found = true; CHECK(slurp(d.path() / "memory.max") == std::to_string(64ll << 20)); CHECK(slurp(d.path() / "pids.max") == "256");
        HAS(slurp(d.path() / "cgroup.procs"), ""); }
    CHECK(found); }

// ---- tty mode ----
// A pty pair stands in for /dev/tty1: the test holds the master (the "user"),
// the Host opens the slave as its console.
struct FakeConsole {
    int master = -1; std::string slave;
    FakeConsole() { master = posix_openpt(O_RDWR | O_NOCTTY); grantpt(master); unlockpt(master); slave = ptsname(master); }
    ~FakeConsole() { close(master); }
    void type(const std::string& s) { (void)!write(master, s.data(), s.size()); }
    std::string drain(int ms) {  // everything the user would see
        std::string out; char buf[4096];
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            struct pollfd p{master, POLLIN, 0};
            if (poll(&p, 1, 50) > 0) { ssize_t r = read(master, buf, sizeof buf); if (r <= 0) break; out.append(buf, r); }
        }
        return out;
    }
};
TEST(tty_runs_child) { FakeConsole con; Config c; c.root = ""; c.tty_path = con.slave; Host h(c);
    std::string seen; std::thread t([&] { seen = con.drain(1500); });
    auto b = h.call("spawn", {{"exe", "/bin/echo"}, {"argv", "via-pty"}, {"mode", "tty"}}).body;
    t.join(); HAS(b, "exit=0"); HAS(b, "via-pty"); HAS(seen, "via-pty"); }
TEST(tty_input_reaches_child) { FakeConsole con; Config c; c.root = ""; c.tty_path = con.slave; Host h(c);
    std::string seen;
    std::thread t([&] { usleep(300000); con.type("abc\n"); usleep(200000); con.type("\x04"); seen = con.drain(1000); });
    auto b = h.call("spawn", {{"exe", "/bin/cat"}, {"mode", "tty"}, {"timeout_s", "10"}}).body;
    t.join(); HAS(b, "exit=0"); HAS(b, "abc"); }
TEST(tty_escape_chord) { FakeConsole con; Config c; c.root = ""; c.tty_path = con.slave; Host h(c);
    std::thread t([&] { usleep(300000); con.type("\x1d\x1d"); con.drain(500); });
    auto t0 = std::chrono::steady_clock::now();
    auto b = h.call("spawn", {{"exe", "/bin/sleep"}, {"argv", "30"}, {"mode", "tty"}}).body;
    t.join(); HAS(b, "escape"); HAS(b, "signal=9");
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)); }
TEST(tty_single_ctrl_bracket_passes_through) { FakeConsole con; Config c; c.root = ""; c.tty_path = con.slave; Host h(c);
    std::thread t([&] { usleep(300000); con.type("\x1dx\n"); usleep(200000); con.type("\x04"); con.drain(800); });
    auto b = h.call("spawn", {{"exe", "/bin/cat"}, {"mode", "tty"}}).body;
    t.join(); HAS(b, "exit=0"); LACKS(b, "escape"); }

// ---- dev isolation ----
static const char* kLsRoot =
    "#include <stdio.h>\n#include <dirent.h>\n"
    "int main(void){ DIR *d = opendir(\"/\"); struct dirent *e; if(!d) return 9;\n"
    "  while((e = readdir(d))) printf(\"[%s]\\n\", e->d_name);\n"
    "  FILE *f = fopen(\"/proc/self/status\", \"r\"); printf(f ? \"proc-ok\\n\" : \"proc-missing\\n\");\n"
    "  printf(\"uid=%d\\n\", (int)getuid()); return 0; }\n";
TEST(isolated_child_sees_village) { Env e; e.cfg.isolate = true; Host h(e.cfg);
    auto c = h.call("compile", {{"lang", "c"}, {"name", "lsroot"}, {"source", std::string("#include <unistd.h>\n") + kLsRoot}}).body;
    HAS(c, "exit=0");
    auto b = h.call("spawn", {{"exe", "/generated/bin/lsroot"}}).body;
    HAS(b, "exit=0"); HAS(b, "[generated]"); HAS(b, "[store]"); LACKS(b, "[usr]"); HAS(b, "proc-ok"); HAS(b, "uid=0"); }
TEST(isolated_background_logs) { Env e; e.cfg.isolate = true; Host h(e.cfg);
    h.call("compile", {{"lang", "c"}, {"name", "lsroot"}, {"source", std::string("#include <unistd.h>\n") + kLsRoot}});
    auto b = h.call("spawn", {{"exe", "/generated/bin/lsroot"}, {"mode", "background"}}).body;
    auto w = h.call("wait", {{"pid", pid_of(b)}}).body; HAS(w, "exit=0"); HAS(w, "[state]"); }

// ---- wait ----
TEST(wait_background) { Env e; Host h(e.cfg); fs::create_directories(e.root / "bin"); fs::create_symlink("/bin/echo", e.root / "bin/echo");
    auto b = h.call("spawn", {{"exe", "/bin/echo"}, {"argv", "done-it"}, {"mode", "background"}}).body;
    auto w = h.call("wait", {{"pid", pid_of(b)}}).body; HAS(w, "exit=0"); HAS(w, "done-it"); }
TEST(wait_signal) { Env e; Host h(e.cfg); fs::create_directories(e.root / "bin"); fs::create_symlink("/bin/sleep", e.root / "bin/sleep");
    auto b = h.call("spawn", {{"exe", "/bin/sleep"}, {"argv", "30"}, {"mode", "background"}}).body;
    auto w = h.call("wait", {{"pid", pid_of(b)}, {"signal", "TERM"}}).body; HAS(w, "signal=15"); }
TEST(wait_unknown) { Env e; Host h(e.cfg); HAS(h.call("wait", {{"pid", "999999"}}).body, "error:"); }

// ---- compile ----
static const char* kHello = "#include <stdio.h>\nint main(void){ printf(\"hi from tcc\\n\"); return 3; }\n";
TEST(compile_lang) { Env e; Host h(e.cfg); HAS(h.call("compile", {{"lang", "rust"}, {"name", "x"}, {"source", "fn main(){}"}}).body, "error:"); }
TEST(compile_store_layout) { Env e; Host h(e.cfg); h.set_turn(4);
    auto b = h.call("compile", {{"lang", "c"}, {"name", "hello"}, {"source", kHello}}).body;
    HAS(b, "exit=0"); HAS(b, "sha256:");
    std::string hex = pk::sha256_hex(std::string("c\n") + "\n" + kHello);
    fs::path d = e.root / ("store/sha256:" + hex);
    HAS(b, hex);
    CHECK(slurp(d / "source.c") == kHello); CHECK(fs::exists(d / "bin"));
    auto m = slurp(d / "manifest.json"); HAS(m, "\"name\": \"hello\""); HAS(m, "\"turn\": 4"); HAS(m, "\"model\": \"test-model\"");
    HAS(m, "\"timestamp\""); HAS(m, hex);
    CHECK(fs::is_symlink(e.root / "generated/bin/hello"));
    std::error_code ec;
    CHECK(fs::read_symlink(e.root / "generated/bin/hello", ec) == fs::path("/store/sha256:" + hex + "/bin")); }
TEST(compile_cached) { Env e; Host h(e.cfg);
    h.call("compile", {{"lang", "c"}, {"name", "hello"}, {"source", kHello}});
    HAS(h.call("compile", {{"lang", "c"}, {"name", "hello2"}, {"source", kHello}}).body, "cached");
    CHECK(fs::is_symlink(e.root / "generated/bin/hello2")); }
TEST(compile_error) { Env e; Host h(e.cfg);
    auto b = h.call("compile", {{"lang", "c"}, {"name", "bad"}, {"source", "int main( { return }"}}).body;
    LACKS(b, "exit=0"); HAS(b, "exit="); HAS(b, "error");
    int n = 0; for (auto& d : fs::directory_iterator(e.root / "store")) { (void)d; ++n; } CHECK(n == 0);
    CHECK(!fs::exists(e.root / "generated/bin/bad")); }
TEST(compile_then_spawn) { Env e; Host h(e.cfg);
    h.call("compile", {{"lang", "c"}, {"name", "hello"}, {"source", kHello}});
    auto b = h.call("spawn", {{"exe", "/generated/bin/hello"}}).body; HAS(b, "exit=3"); HAS(b, "hi from tcc"); }
TEST(compile_bad_name) { Env e; Host h(e.cfg);
    HAS(h.call("compile", {{"lang", "c"}, {"name", "../x"}, {"source", kHello}}).body, "error:"); }
TEST(sha256_vectors) {
    CHECK(pk::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(pk::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(pk::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(pk::sha256_hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"); }

// ---- snapshot ----
TEST(snapshot_create) { Env e; Host h(e.cfg); spit(e.root / "generated/a", "1");
    CHECK(h.call("snapshot", {}).body == "snapshot=1"); CHECK(h.call("snapshot", {}).body == "snapshot=2");
    CHECK(slurp(e.root / "snapshots/1/generated/a") == "1"); }
TEST(snapshot_rollback) { Env e; Host h(e.cfg); spit(e.root / "generated/a", "1"); spit(e.root / "state/t", "x");
    h.call("snapshot", {});
    spit(e.root / "generated/a", "2"); spit(e.root / "generated/new", "n"); spit(e.root / "state/t", "y");
    LACKS(h.call("snapshot", {{"rollback", "1"}}).body, "error");
    CHECK(slurp(e.root / "generated/a") == "1"); CHECK(!fs::exists(e.root / "generated/new")); CHECK(slurp(e.root / "state/t") == "x"); }
TEST(rollback_kills_later_children) { Env e; Host h(e.cfg); fs::create_directories(e.root / "bin"); fs::create_symlink("/bin/sleep", e.root / "bin/sleep");
    auto b0 = h.call("spawn", {{"exe", "/bin/sleep"}, {"argv", "30"}, {"mode", "background"}}).body;
    h.call("snapshot", {});
    auto b1 = h.call("spawn", {{"exe", "/bin/sleep"}, {"argv", "30"}, {"mode", "background"}}).body;
    pid_t p0 = std::atoi(b0.substr(b0.find("pid=") + 4).c_str()), p1 = std::atoi(b1.substr(b1.find("pid=") + 4).c_str());
    auto r = h.call("snapshot", {{"rollback", "1"}}).body; HAS(r, "killed " + std::to_string(p1));
    usleep(100000);
    CHECK(kill(p0, 0) == 0); CHECK(kill(p1, 0) != 0);
    h.call("wait", {{"pid", std::to_string(p0)}, {"signal", "KILL"}}); }
TEST(rollback_unknown) { Env e; Host h(e.cfg); HAS(h.call("snapshot", {{"rollback", "42"}}).body, "error:"); }

// ---- fetch ----
TEST(fetch_offline) { Env e; Host h(e.cfg); HAS(h.call("fetch", {{"url", "http://x"}}).body, "error: fetch: not available offline"); }

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    for (auto& [n, f] : registry()) {
        if (argc > 1 && n.find(argv[1]) == std::string::npos) continue;
        g_cur = n; int before = g_fail; ++g_run; f();
        std::printf("%s %s\n", g_fail == before ? "ok  " : "FAIL", n.c_str());
    }
    std::printf("%d tests, %d failed checks\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
