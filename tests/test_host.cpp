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
#include <atomic>
#include <fcntl.h>
#include <poll.h>
#include <chrono>
#include <signal.h>
#include <sstream>
#include <sys/resource.h>
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
TEST(mounts_map_model_paths) { Env e; fs::path sys = e.root / "hostside"; spit(sys / "inc/time.h", "struct tm;");
    e.cfg.mounts = {{"/usr/lib/potemkin", sys.string()}}; Host h(e.cfg);
    CHECK(h.call("read", {{"path", "/usr/lib/potemkin/inc/time.h"}}).body == "struct tm;");
    CHECK(h.real("/usr/lib/potemkinx") == e.root.string() + "/usr/lib/potemkinx"); }
TEST(read_proc) { Config c; c.root = ""; Host h(c);
    auto r = h.call("read", {{"path", "/proc/self/status"}}); HAS(r.body, "Name:"); }

// review fixes
static bool utf8_ok(const std::string& s) {  // strict: no overlongs, no surrogates, <= U+10FFFF
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        if (!n || i + n > s.size()) return false;
        for (int k = 1; k < n; ++k) if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}
TEST(read_endless_device_is_bounded) { Config c; c.root = ""; Host h(c);
    auto t0 = std::chrono::steady_clock::now();
    auto r = h.call("read", {{"path", "/dev/zero"}, {"len", "16"}});
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)); HAS(r.body, "00 00 00 00");
    auto u = h.call("read", {{"path", "/dev/urandom"}});  // no len: still bounded
    CHECK(u.body.size() < 20000); }
TEST(read_offset_does_not_slurp) { Env e; Host h(e.cfg);
    std::string big(3 << 20, 'x'); big.replace(2 << 20, 5, "HELLO"); spit(e.root / "big", big);
    CHECK(h.call("read", {{"path", "/big"}, {"offset", std::to_string(2 << 20)}, {"len", "5"}}).body == "HELLO"); }
TEST(read_cut_keeps_utf8_whole) { Env e; e.cfg.max_result_bytes = 100; Host h(e.cfg);
    std::string s(99, 'a'); s += "\u2500\u2500\u2500"; spit(e.root / "f", s);
    auto r = h.call("read", {{"path", "/f"}}); CHECK(r.truncated); CHECK(utf8_ok(r.body)); }
TEST(spawn_binary_output_is_valid_utf8) { Config c; c.root = ""; Host h(c);
    auto r = h.call("spawn", {{"exe", "/usr/bin/head"}, {"argv", "-c 300 /dev/urandom"}});
    HAS(r.body, "exit=0"); CHECK(utf8_ok(r.body)); }
TEST(spawn_capture_keeps_real_tail) { Config c; c.root = ""; Host h(c);
    auto r = h.call("spawn", {{"exe", "/usr/bin/seq"}, {"argv", "1 100000"}});
    HAS(r.body, "exit=0"); HAS(r.body, "\n100000\n"); HAS(r.body, "omitted"); }
TEST(spawn_capture_returns_when_child_exits) { Config c; c.root = ""; Host h(c);
    auto t0 = std::chrono::steady_clock::now();
    auto r = h.call("spawn", {{"exe", "/bin/sh"}, {"argv", "-c 'sleep 3 & echo started'"}, {"timeout_s", "20"}});
    HAS(r.body, "exit=0"); HAS(r.body, "started"); LACKS(r.body, "timeout");
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)); }
TEST(compile_opts_in_order) { Env e; Host h(e.cfg);
    auto b = h.call("compile", {{"lang", "c"}, {"name", "x"}, {"opts", "-D X=3 -D Y=4"},
                                {"source", "#include <stdio.h>\nint main(void){ printf(\"%d\\n\", X*Y); return 0; }\n"}}).body;
    HAS(b, "exit=0");
    HAS(h.call("spawn", {{"exe", "/generated/bin/x"}}).body, "12"); }
TEST(snapshot_skips_special_files) { Env e; Host h(e.cfg); spit(e.root / "state/t", "x");
    mkfifo((e.root / "state/fifo").c_str(), 0644);
    CHECK(h.call("snapshot", {}).body == "snapshot=1"); CHECK(slurp(e.root / "snapshots/1/state/t") == "x"); }
TEST(snapshot_ids_continue_across_restarts) { Env e;
    { Host h(e.cfg); h.call("snapshot", {}); h.call("snapshot", {}); }
    Host h(e.cfg); CHECK(h.call("snapshot", {}).body == "snapshot=3");
    LACKS(h.call("snapshot", {{"rollback", "1"}}).body, "error"); }
TEST(kill_uses_cgroup_kill) { Env e; fs::path cg = e.root / "cg"; fs::create_directories(cg); e.cfg.cgroup_root = cg.string();
    fs::create_directories(e.root / "bin"); fs::create_symlink("/bin/sleep", e.root / "bin/sleep");
    Host h(e.cfg);
    auto b = h.call("spawn", {{"exe", "/bin/sleep"}, {"argv", "30"}, {"mode", "background"}}).body;
    std::string pid = pid_of(b);
    h.call("wait", {{"pid", pid}, {"signal", "KILL"}});
    CHECK(slurp(cg / "potemkin" / pid / "cgroup.kill") == "1" || !fs::exists(cg / "potemkin" / pid)); }
TEST(cgroup_removed_after_exit) { Env e; fs::path cg = e.root / "cg"; fs::create_directories(cg); e.cfg.cgroup_root = cg.string();
    Config c = e.cfg; c.root = ""; Host h(c);
    h.call("spawn", {{"exe", "/bin/true"}});
    int n = 0; std::error_code ec; for (auto& d : fs::directory_iterator(cg / "potemkin", ec)) { (void)d; ++n; }
    CHECK(n == 0); }
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
    Config c = e.cfg; c.root = ""; c.snapshot_dir = e.root.string() + "/snapshots"; Host h(c);
    fs::create_directories(e.root / "state/log");
    Config c2 = e.cfg; Host h2(c2);  // background needs /state/log under a root
    fs::create_directories(e.root / "bin"); fs::create_symlink("/bin/sleep", e.root / "bin/sleep");
    auto b = h2.call("spawn", {{"exe", "/bin/sleep"}, {"argv", "30"}, {"mode", "background"}, {"mem_mb", "64"}}).body;
    fs::path d = cg / "potemkin" / pid_of(b);
    CHECK(slurp(d / "memory.max") == std::to_string(64ll << 20)); CHECK(slurp(d / "pids.max") == "256");
    CHECK(slurp(d / "cgroup.procs") == pid_of(b));
    h2.call("wait", {{"pid", pid_of(b)}, {"signal", "KILL"}});
    CHECK(!fs::exists(d)); }
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

TEST(tty_child_closing_terminal_does_not_spin) { FakeConsole con; Config c; c.root = ""; c.tty_path = con.slave; Host h(c);
    std::thread t([&] { con.drain(2500); });
    struct rusage r0, r1; getrusage(RUSAGE_SELF, &r0);
    auto b = h.call("spawn", {{"exe", "/bin/sh"}, {"argv", "-c 'exec 0<&- 1>&- 2>&-; sleep 2'"}, {"mode", "tty"}}).body;
    getrusage(RUSAGE_SELF, &r1); t.join();
    HAS(b, "exit=0");
    double cpu = (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) + (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) +
                 ((r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) + (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec)) / 1e6;
    CHECK(cpu < 0.5); }

// ---- secrets never leave through tool results or the console ----
static const char* kKey = "sk-or-v1-0123456789abcdef";
TEST(secret_scrubbed_from_read) { Env e; e.cfg.secrets = {kKey}; Host h(e.cfg);
    spit(e.root / "cmdline", std::string("console=ttyS0 api_key=") + kKey + " llm=api\n");
    auto b = h.call("read", {{"path", "/cmdline"}}).body; LACKS(b, kKey); HAS(b, "api_key=*************************"); }
TEST(secret_scrubbed_from_capture) { Config c; c.root = ""; c.secrets = {kKey}; Host h(c);
    auto b = h.call("spawn", {{"exe", "/bin/echo"}, {"argv", std::string("key is ") + kKey}}).body;
    LACKS(b, kKey); HAS(b, "key is [redacted]"); }
TEST(secret_scrubbed_from_tty_console) { FakeConsole con; Config c; c.root = ""; c.tty_path = con.slave; c.secrets = {kKey}; Host h(c);
    std::string seen; std::thread t([&] { seen = con.drain(1500); });
    auto b = h.call("spawn", {{"exe", "/bin/echo"}, {"argv", kKey}, {"mode", "tty"}}).body;
    t.join(); LACKS(seen, kKey); HAS(seen, "[redacted]"); LACKS(b, kKey); }
TEST(secret_split_across_chunks_scrubbed) { FakeConsole con; Config c; c.root = ""; c.tty_path = con.slave; c.secrets = {kKey}; Host h(c);
    std::string seen; std::thread t([&] { seen = con.drain(2500); });
    std::string half1(kKey, 10), half2(kKey + 10);
    h.call("spawn", {{"exe", "/bin/sh"}, {"argv", "-c 'printf %s " + half1 + "; sleep 0.5; printf %s " + half2 + "'"}, {"mode", "tty"}});
    t.join(); LACKS(seen, kKey); HAS(seen, "[redacted]"); }

// ---- review round 2 ----
TEST(secret_scrubbed_from_hexdump) { Env e; e.cfg.secrets = {kKey}; Host h(e.cfg);
    std::string env = std::string("PATH=/\0api_key=", 15) + kKey + std::string("\0TERM=linux\0", 12);
    spit(e.root / "environ", env);
    auto b = h.call("read", {{"path", "/environ"}}).body;
    HAS(b, "[binary: hexdump"); LACKS(b, "30 31 32 33 34 35"); LACKS(b, "0123456789"); }
TEST(secret_scrubbed_across_read_windows) { Env e; e.cfg.secrets = {kKey}; Host h(e.cfg);
    spit(e.root / "f", std::string("xx ") + kKey + " yy");
    auto a = h.call("read", {{"path", "/f"}, {"offset", "0"}, {"len", "15"}}).body;
    auto b = h.call("read", {{"path", "/f"}, {"offset", "15"}, {"len", "40"}}).body;
    LACKS(a + b, "sk-or-v1-0123"); LACKS(a + b, "456789abcdef"); HAS(a, "xx "); HAS(b, " yy"); }
TEST(snapshot_fails_cleanly_on_copy_error) { Env e; Host h(e.cfg); spit(e.root / "state/secretfile", "x");
    chmod((e.root / "state/secretfile").c_str(), 0000);
    auto b = h.call("snapshot", {}).body; chmod((e.root / "state/secretfile").c_str(), 0644);
    if (::geteuid() != 0) { HAS(b, "error"); CHECK(!fs::exists(e.root / "snapshots/1")); } }
TEST(snapshot_excludes_logs) { Env e; Host h(e.cfg); spit(e.root / "state/log/77", std::string(1000, 'l')); spit(e.root / "state/t", "x");
    h.call("snapshot", {}); CHECK(fs::exists(e.root / "snapshots/1/state/t")); CHECK(!fs::exists(e.root / "snapshots/1/state/log/77"));
    spit(e.root / "state/log/77", "newer"); h.call("snapshot", {{"rollback", "1"}});
    CHECK(slurp(e.root / "state/log/77") == "newer"); }
TEST(snapshots_pruned_to_last_20) { Env e; Host h(e.cfg);
    for (int i = 0; i < 25; ++i) h.call("snapshot", {});
    int n = 0; for (auto& d : fs::directory_iterator(e.root / "snapshots")) { (void)d; ++n; }
    CHECK(n == 20); CHECK(!fs::exists(e.root / "snapshots/5")); CHECK(fs::exists(e.root / "snapshots/25")); }
TEST(cancel_interrupts_capture) { Config c; c.root = ""; std::atomic<bool> cancel{false}; c.cancel = &cancel; Host h(c);
    std::thread t([&] { usleep(300000); cancel = true; });
    auto t0 = std::chrono::steady_clock::now();
    auto b = h.call("spawn", {{"exe", "/bin/sleep"}, {"argv", "30"}, {"timeout_s", "0"}}).body;
    t.join(); HAS(b, "interrupted"); CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)); }
TEST(cancel_interrupts_wait) { Env e; std::atomic<bool> cancel{false}; e.cfg.cancel = &cancel; Host h(e.cfg);
    fs::create_directories(e.root / "bin"); fs::create_symlink("/bin/sleep", e.root / "bin/sleep");
    auto b = h.call("spawn", {{"exe", "/bin/sleep"}, {"argv", "30"}, {"mode", "background"}}).body;
    std::thread t([&] { usleep(300000); cancel = true; });
    auto t0 = std::chrono::steady_clock::now();
    auto w = h.call("wait", {{"pid", pid_of(b)}, {"timeout_s", "60"}}).body;
    t.join(); HAS(w, "interrupted"); CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
    cancel = false; h.call("wait", {{"pid", pid_of(b)}, {"signal", "KILL"}}); }
TEST(leftover_cgroups_from_previous_life_are_killed) { Env e; fs::path cg = e.root / "cg"; e.cfg.cgroup_root = cg.string();
    fs::create_directories(cg / "potemkin/424242"); spit(cg / "potemkin/424242/cgroup.procs", "424242");
    spit(e.root / "state/procs", "424242 background running turn=3 daemon\n");
    Host h(e.cfg);
    CHECK(!fs::exists(cg / "potemkin/424242")); LACKS(slurp(e.root / "state/procs"), "424242"); }

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
TEST(isolated_child_sees_mounts) { Env e; e.cfg.isolate = true;
    fs::path side = e.root.string() + "-side";
    fs::create_directories(side); std::ofstream(side / "marker") << "from-the-sysroot";
    e.cfg.mounts = {{"/usr/lib/potemkin", side.string()}};
    Host h(e.cfg);
    auto c = h.call("compile", {{"lang", "c"}, {"name", "catm"}, {"source",
        "#include <stdio.h>\nint main(void){ FILE *f = fopen(\"/usr/lib/potemkin/marker\", \"r\"); char b[64] = {0};\n"
        "  if (!f) { printf(\"missing\\n\"); return 1; } fgets(b, sizeof b, f); printf(\"%s\\n\", b); return 0; }\n"}}).body;
    HAS(c, "exit=0");
    auto b = h.call("spawn", {{"exe", "/generated/bin/catm"}}).body;
    HAS(b, "exit=0"); HAS(b, "from-the-sysroot");
    fs::remove_all(side); }
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
TEST(fetch_online_text) { Env e; e.cfg.netboot = true;
    e.cfg.fetcher = [](const std::string& url, std::string& err) { (void)err; return "got " + url; };
    Host h(e.cfg); CHECK(h.call("fetch", {{"url", "http://x/y"}}).body == "got http://x/y"); }
TEST(fetch_online_error) { Env e; e.cfg.netboot = true;
    e.cfg.fetcher = [](const std::string&, std::string& err) { err = "HTTP 404"; return std::string(); };
    Host h(e.cfg); HAS(h.call("fetch", {{"url", "http://x"}}).body, "error: fetch: HTTP 404"); }
TEST(fetch_binary_is_hexdump) { Env e; e.cfg.netboot = true;
    e.cfg.fetcher = [](const std::string&, std::string&) { return std::string("\x7f" "ELF\x00\x01\xff", 7); };
    Host h(e.cfg); HAS(h.call("fetch", {{"url", "http://x"}}).body, "[binary: hexdump"); }
TEST(fetch_caps) { Env e; e.cfg.netboot = true; e.cfg.max_result_bytes = 50;
    e.cfg.fetcher = [](const std::string&, std::string&) { return std::string(500, 'z'); };
    Host h(e.cfg); auto r = h.call("fetch", {{"url", "http://x"}}); CHECK(r.truncated); }
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
