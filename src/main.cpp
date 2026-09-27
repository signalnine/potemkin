// q27-init: the console, the model, and the eight tools. Spawned by /sbin/init
// on /dev/tty1; also runs on an ordinary terminal for development.
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <termios.h>
#include <unistd.h>

#include "harness/harness.h"
#include "host/host.h"
#include "api/backend_api.h"
#include "q27/backend_q27.h"

namespace {

std::atomic<bool>* g_cancel = nullptr;

void on_sigint(int) {
    if (g_cancel) g_cancel->store(true);
}

class TtyConsole : public pk::Console {
public:
    explicit TtyConsole(int fd) : fd_(fd) {}
    void think(const std::string& s) override { style(true); put(s); }
    void text(const std::string& s) override { style(false); put(s); }
    void say(const std::string& s) override { style(false); put(s + "\n"); }
    void note(const std::string& s) override { style(true); put(s + "\n"); style(false); }
    // Thinking is dim; switch attributes only on a change, not per token.
    void style(bool dim) {
        if (dim == dim_) return;
        dim_ = dim;
        put(dim ? "\x1b[2m" : "\x1b[0m");
    }
    void put(const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '\n') o += '\r'; o += c; }  // survive a raw tty
        size_t off = 0;
        while (off < o.size()) {
            ssize_t w = ::write(fd_, o.data() + off, o.size() - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            off += w;
        }
    }

private:
    int fd_;
    bool dim_ = false;
};

// Blocking line read in cooked mode. Returns false on EOF.
bool read_line(int fd, std::string& line) {
    line.clear();
    char c;
    for (;;) {
        ssize_t r = ::read(fd, &c, 1);
        if (r < 0 && errno == EINTR) { line.clear(); return true; }  // Ctrl-C at the prompt: drop the line
        // Ctrl-D on a terminal is not a way out: reloading the weights takes
        // minutes. Only a real EOF (a pipe, a closed pty) ends the loop.
        if (r == 0 && line.empty() && ::isatty(fd)) { (void)!::write(fd, "\r\n> ", 4); continue; }
        if (r <= 0) return !line.empty();
        if (c == '\n') return true;
        line += c;
    }
}

std::map<std::string, std::string> kernel_cmdline() {
    std::map<std::string, std::string> out;
    std::ifstream f("/proc/cmdline");
    std::string tok;
    while (f >> tok) {
        size_t eq = tok.find('=');
        if (eq != std::string::npos) out[tok.substr(0, eq)] = tok.substr(eq + 1);
    }
    return out;
}

void usage() {
    std::fprintf(stderr,
                 "usage: q27-init --model M.q27 --tok T.tok [--tty /dev/tty1|-] [--root DIR]\n"
                 "                [--sysroot DIR] [--cgroup /sys/fs/cgroup] [--ctx N] [--fixed-stack-gb G]\n"
                 "                [--prefix-cache DIR] [--no-think] [--engine-log FILE] [--dflash2 PACK.d2w]\n"
                 "                [--think-budget N]\n"
                 "                [--llm cuda|api] [--api-url URL] [--api-model NAME]  (key: PK_API_KEY)\n"
                 "kernel cmdline supplies llm= model= api_url= api_key= api_model=.\n");
}

}  // namespace

int main(int argc, char** argv) {
    pk::Q27Opts qo;
    std::string tty = "-", root, sysroot = "/usr/lib/potemkin", cgroup, engine_log;
    auto kc = kernel_cmdline();
    std::string llm = kc.count("llm") ? kc["llm"] : "cuda";
    pk::ApiOpts ao;
    if (kc.count("api_url")) ao.url = kc["api_url"];
    if (kc.count("api_key")) ao.key = kc["api_key"];  // on the kernel cmdline, as designed
    if (kc.count("api_model")) ao.model = kc["api_model"];
    if (kc.count("api_context")) ao.context = std::atoi(kc["api_context"].c_str());  // compaction triggers at 3/4 of it
    if (kc.count("api_max_tokens")) ao.max_tokens = std::atoi(kc["api_max_tokens"].c_str());
    if (const char* k = getenv("PK_API_KEY")) ao.key = k;  // dev: keep keys out of argv
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
        if (a == "--model") qo.model = next();
        else if (a == "--tok") qo.tok = next();
        else if (a == "--tty") tty = next();
        else if (a == "--root") root = next();
        else if (a == "--sysroot") sysroot = next();
        else if (a == "--cgroup") cgroup = next();
        else if (a == "--ctx") qo.ctx = std::atoi(next().c_str());
        else if (a == "--fixed-stack-gb") qo.fixed_stack_gb = std::atof(next().c_str());
        else if (a == "--prefix-cache") qo.prefix_cache = next();
        else if (a == "--dflash2") qo.dflash2 = next();
        else if (a == "--no-think") qo.think = false;
        else if (a == "--think-budget") qo.think_budget = std::atoi(next().c_str());
        else if (a == "--llm") llm = next();
        else if (a == "--engine-log") engine_log = next();
        else if (a == "--api-url") ao.url = next();
        else if (a == "--api-model") ao.model = next();
        else { usage(); return 2; }
    }
    if (llm != "cuda" && llm != "api") { std::fprintf(stderr, "q27-init: llm=%s not built yet\n", llm.c_str()); return 2; }
    if (kc.count("think_budget") && qo.think_budget < 0) qo.think_budget = std::atoi(kc["think_budget"].c_str());
    if (llm == "cuda") {
        // Image defaults: model=qwen|bonsai picks the weights on the persistent disk.
        std::string which = kc.count("model") ? kc["model"] : "bonsai";
        // Unbounded, Bonsai plans a shell for 65K tokens and writes nothing;
        // at 8K it writes one in under two minutes. Qwen keeps the recipe.
        if (which == "bonsai" && qo.think_budget < 0) qo.think_budget = 8192;
        if (qo.model.empty()) qo.model = "/models/" + which + ".q27";
        if (qo.tok.empty()) qo.tok = "/models/qwen38.tok";
        std::string d2 = "/models/" + which + "-dflash2.d2w";
        if (qo.dflash2.empty() && ::access(d2.c_str(), R_OK) == 0) qo.dflash2 = d2;
    }

    // q27 narrates to stderr; that belongs in a log, not on the console.
    if (!engine_log.empty()) {
        int lf = ::open(engine_log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (lf >= 0) { ::dup2(lf, 2); ::close(lf); }
    }
    // The inference process must outlive whatever the model writes.
    { std::ofstream f("/proc/self/oom_score_adj"); f << "-1000\n"; }

    int fd = 1, in = 0;
    if (tty != "-") {
        fd = ::open(tty.c_str(), O_RDWR);
        if (fd < 0) { std::perror(tty.c_str()); return 1; }
        in = fd;
    }
    TtyConsole con(fd);

    pk::Config hc;
    hc.root = root;
    hc.tty_path = tty == "-" ? std::string(::ttyname(0) ? ::ttyname(0) : "/dev/tty") : tty;
    hc.cgroup_root = cgroup;
    std::string m = sysroot + "/usr/lib/x86_64-linux-musl", t = sysroot + "/usr/lib/x86_64-linux-gnu/tcc";
    hc.cc = {sysroot + "/usr/bin/tcc", "-nostdinc", "-nostdlib", "-static",
             "-I" + sysroot + "/usr/include/x86_64-linux-musl", "-I" + t + "/include",
             m + "/crt1.o", m + "/crti.o", "@SRC@", m + "/libc.a", t + "/libtcc1.a", m + "/crtn.o"};
    hc.model_name = llm == "api" ? ao.model : qo.model.substr(qo.model.rfind('/') + 1);
    hc.netboot = llm == "api";
    // The key rides on the kernel cmdline (by design), so the model can read
    // it from /proc/cmdline or dmesg. It never leaves in a tool result.
    if (ao.key.size() >= 8) hc.secrets.push_back(ao.key);
    if (hc.netboot) {
        std::string ca = ao.ca_file;
        hc.fetcher = [ca](const std::string& url, std::string& err) { return pk::http_fetch(url, err, ca); };
    }
    hc.isolate = !root.empty();  // dev: programs see the village as /, like the image
    if (!root.empty() && sysroot != "/usr/lib/potemkin") hc.mounts = {{"/usr/lib/potemkin", sysroot}};
    // The image ships glibc (tcc and libcuda load it); the dev village borrows
    // the host's, read-only to an unprivileged user.
    if (!root.empty())
        for (const char* d : {"/lib64", "/lib/x86_64-linux-gnu"})
            if (access(d, F_OK) == 0) hc.mounts.push_back({d, d});

    con.say("loading " + hc.model_name + " ...");
    std::unique_ptr<pk::Backend> be;
    try {
        be = llm == "api" ? pk::make_api_backend(ao) : pk::make_q27_backend(qo);
    } catch (const std::exception& e) {
        con.say(std::string("model load failed: ") + e.what());
        return 1;
    }

    pk::Host host(hc);
    pk::HarnessConfig cfg;
    cfg.system_prompt = pk::kSystemPrompt;
    pk::Harness h(*be, host, con, cfg);
    g_cancel = &h.cancel;
    struct sigaction sa {};
    sa.sa_handler = on_sigint;  // no SA_RESTART: a Ctrl-C at the prompt interrupts the read
    sigaction(SIGINT, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    h.boot();
    std::string line;
    for (;;) {
        con.put("\n> ");
        if (!read_line(in, line)) break;
        if (h.handle_line(line) == pk::Action::Reboot) return 3;  // init reboots on 3
    }
    return 0;
}
