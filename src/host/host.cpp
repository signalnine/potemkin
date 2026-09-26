// Host primitives. See CONTRACT.md for the behavior each tool promises.
#include "host.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace pk {

namespace {

ToolResult err(const std::string& msg) { return {"error: " + msg}; }

struct ArgError { std::string msg; };

const std::string& need(const Args& a, const char* tool, const char* key) {
    auto it = a.find(key);
    if (it == a.end()) throw ArgError{std::string(tool) + ": missing " + key};
    return it->second;
}
std::string opt(const Args& a, const char* key, const std::string& def) {
    auto it = a.find(key);
    return it == a.end() || it->second.empty() ? def : it->second;
}
long long to_int(const std::string& s, const char* key) {
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (s.empty() || *end || errno) throw ArgError{std::string(key) + ": not an integer: " + s};
    return v;
}
void need_abs(const std::string& p) {
    if (p.empty() || p[0] != '/') throw ArgError{"path must be absolute: " + p};
}

bool valid_utf8(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len) return false;
        if (c == 0) return false;
        // A multi-byte sequence cut at the end of a page is still text.
        if (i + len > n) return true;
        for (int k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
        i += len;
    }
    return true;
}

std::string hexdump(const std::string& s, size_t base) {
    std::string out;
    char buf[128];
    for (size_t i = 0; i < s.size(); i += 16) {
        std::snprintf(buf, sizeof buf, "%08zx ", base + i);
        out += buf;
        std::string asc;
        for (size_t k = 0; k < 16; ++k) {
            if (i + k < s.size()) {
                unsigned char c = s[i + k];
                std::snprintf(buf, sizeof buf, " %02x", c);
                out += buf;
                asc += (c >= 32 && c < 127) ? char(c) : '.';
            } else {
                out += "   ";
            }
        }
        out += "  |" + asc + "|\n";
    }
    return out;
}

bool read_all(const std::string& path, std::string& out) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[65536];
    ssize_t r;
    while ((r = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, r);
    ::close(fd);
    return r == 0;
}

bool write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << data;
    return bool(f);
}

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o + "\"";
}

int parse_signal(const std::string& s) {
    static const std::map<std::string, int> names = {
        {"TERM", SIGTERM}, {"KILL", SIGKILL}, {"INT", SIGINT}, {"HUP", SIGHUP},
        {"STOP", SIGSTOP}, {"CONT", SIGCONT}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"QUIT", SIGQUIT}};
    std::string u = s;
    if (u.rfind("SIG", 0) == 0) u = u.substr(3);
    auto it = names.find(u);
    if (it != names.end()) return it->second;
    return static_cast<int>(to_int(s, "signal"));
}

std::string status_str(int st) {
    if (WIFEXITED(st)) return "exit=" + std::to_string(WEXITSTATUS(st));
    if (WIFSIGNALED(st)) return "signal=" + std::to_string(WTERMSIG(st));
    return "status=" + std::to_string(st);
}

// Tail of a string, cut at a line boundary when possible.
std::string tail(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t start = s.size() - n;
    size_t nl = s.find('\n', start);
    if (nl != std::string::npos && nl + 1 < s.size()) start = nl + 1;
    return "[... " + std::to_string(start) + " bytes omitted]\n" + s.substr(start);
}

void copy_tree(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    if (!fs::exists(from)) return;
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks |
                           fs::copy_options::overwrite_existing);
}

}  // namespace

// ---------------------------------------------------------------- sha256
// FIPS 180-4. Small and local so the store hash needs no crypto library.

std::string sha256_hex(const std::string& data) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    std::string m = data;
    uint64_t bits = uint64_t(data.size()) * 8;
    m += char(0x80);
    while (m.size() % 64 != 56) m += char(0);
    for (int i = 7; i >= 0; --i) m += char((bits >> (i * 8)) & 0xff);
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const unsigned char* b = reinterpret_cast<const unsigned char*>(m.data() + off + i * 4);
            w[i] = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 64);
}

// ---------------------------------------------------------------- argv split

std::vector<std::string> split_argv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool in = false;
    char q = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (q) {
            if (c == q) q = 0;
            else if (c == '\\' && q == '"' && i + 1 < s.size()) cur += s[++i];
            else cur += c;
        } else if (c == '\'' || c == '"') {
            q = c; in = true;
        } else if (c == '\\' && i + 1 < s.size()) {
            cur += s[++i]; in = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (in) { out.push_back(cur); cur.clear(); in = false; }
        } else {
            cur += c; in = true;
        }
    }
    if (in) out.push_back(cur);
    return out;
}

// ---------------------------------------------------------------- Host

Host::Host(Config c) : cfg_(std::move(c)) {}

Host::~Host() {
    for (auto& [pid, p] : procs_)
        if (p.state == "running") { ::kill(-pid, SIGKILL); ::waitpid(pid, nullptr, 0); }
}

std::string Host::real(const std::string& p) const { return cfg_.root + p; }

// Follow symlinks the way the model sees them: absolute targets are model
// paths, so they get the root prefix again. Identity when root is "".
std::string Host::resolve(const std::string& model_path) const {
    std::string p = model_path;
    for (int hops = 0; hops < 40; ++hops) {
        std::error_code ec;
        fs::path r = real(p);
        if (!fs::is_symlink(r, ec)) return r.string();
        fs::path t = fs::read_symlink(r, ec);
        if (ec) return r.string();
        p = t.is_absolute() ? t.string() : (fs::path(p).parent_path() / t).lexically_normal().string();
    }
    return real(p);
}

std::vector<std::string> Host::tool_names() const {
    return {"read", "write", "stat", "spawn", "wait", "compile", "snapshot", "fetch"};
}

ToolResult Host::call(const std::string& name, const Args& args) {
    try {
        reap_nohang();
        if (name == "read") return t_read(args);
        if (name == "write") return t_write(args);
        if (name == "stat") return t_stat(args);
        if (name == "spawn") return t_spawn(args);
        if (name == "wait") return t_wait(args);
        if (name == "compile") return t_compile(args);
        if (name == "snapshot") return t_snapshot(args);
        if (name == "fetch") return t_fetch(args);
        return err("unknown tool " + name);
    } catch (const ArgError& e) {
        return err(e.msg);
    } catch (const std::exception& e) {
        return err(name + ": " + e.what());
    }
}

ToolResult Host::cap(std::string body) const {
    if (body.size() <= cfg_.max_result_bytes) return {std::move(body)};
    size_t n = body.size();
    body.resize(cfg_.max_result_bytes);
    body += "\n[truncated: " + std::to_string(n - cfg_.max_result_bytes) + " more bytes]";
    return {std::move(body), true};
}

// ---------------------------------------------------------------- read

ToolResult Host::t_read(const Args& a) {
    const std::string& path = need(a, "read", "path");
    need_abs(path);
    long long off = to_int(opt(a, "offset", "0"), "offset");
    long long len = a.count("len") && !a.at("len").empty() ? to_int(a.at("len"), "len") : -1;
    if (off < 0) throw ArgError{"offset must be >= 0"};

    std::string data;
    std::error_code ec;
    if (fs::is_directory(real(path), ec)) return err("read: " + path + " is a directory; use stat with list=1");
    if (!read_all(resolve(path), data)) return err("read: " + path + ": " + std::strerror(errno));
    size_t total = data.size();
    size_t start = std::min<size_t>(off, total);
    size_t want = len < 0 ? total - start : std::min<size_t>(len, total - start);

    std::string chunk = data.substr(start, want);
    bool text = valid_utf8(chunk);
    // Hexdump triples the size, so a binary page covers fewer bytes.
    size_t budget = text ? cfg_.max_result_bytes : cfg_.max_result_bytes / 5;
    bool cut = chunk.size() > budget;
    if (cut) chunk.resize(budget);
    size_t end = start + chunk.size();

    std::string body;
    if (text) {
        body = chunk;
    } else {
        body = "[binary: hexdump of bytes " + std::to_string(start) + "-" + std::to_string(end) +
               " of " + std::to_string(total) + "]\n" + hexdump(chunk, start);
    }
    if (cut) {
        body += "\n[truncated: bytes " + std::to_string(start) + "-" + std::to_string(end) + " of " +
                std::to_string(total) + "; read offset=" + std::to_string(end) + " to continue]";
    }
    return {body, cut};
}

// ---------------------------------------------------------------- write

ToolResult Host::t_write(const Args& a) {
    const std::string& path = need(a, "write", "path");
    const std::string& content = need(a, "write", "content");
    need_abs(path);
    fs::path p = real(path);
    fs::create_directories(p.parent_path());
    if (!write_file(p, content)) return err("write: " + path + ": " + std::strerror(errno));
    return {"wrote " + std::to_string(content.size()) + " bytes to " + path};
}

// ---------------------------------------------------------------- stat

ToolResult Host::t_stat(const Args& a) {
    const std::string& path = need(a, "stat", "path");
    need_abs(path);
    bool list = opt(a, "list", "0") != "0";
    std::string p = real(path);
    struct stat st;
    if (::lstat(p.c_str(), &st) != 0) return err("stat: " + path + ": " + std::strerror(errno));

    if (!list) {
        const char* type = S_ISREG(st.st_mode) ? "file" : S_ISDIR(st.st_mode) ? "dir" : S_ISLNK(st.st_mode) ? "symlink"
                         : S_ISCHR(st.st_mode) ? "chardev" : S_ISBLK(st.st_mode) ? "blockdev"
                         : S_ISFIFO(st.st_mode) ? "fifo" : S_ISSOCK(st.st_mode) ? "socket" : "other";
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s size=%lld mode=%04o mtime=%lld", type, (long long)st.st_size,
                      st.st_mode & 07777, (long long)st.st_mtime);
        std::string out = buf;
        if (S_ISLNK(st.st_mode)) out += " target=" + fs::read_symlink(p).string();
        return {out};
    }

    std::error_code ec;
    if (!fs::is_directory(p, ec)) return err("stat: " + path + ": not a directory");
    std::vector<std::string> lines;
    for (auto& e : fs::directory_iterator(p, ec)) {
        std::string n = e.path().filename().string();
        if (e.is_symlink()) n += " -> " + fs::read_symlink(e.path()).string();
        else if (e.is_directory()) n += "/";
        lines.push_back(n);
    }
    if (ec) return err("stat: " + path + ": " + ec.message());
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (auto& l : lines) out += l + "\n";
    return cap(out);
}

// ---------------------------------------------------------------- processes

void Host::write_procs() {
    std::string out;
    for (auto& [pid, p] : procs_)
        out += std::to_string(pid) + " " + p.mode + " " + p.state + " turn=" + std::to_string(p.turn) + " " + p.argv + "\n";
    fs::path f = real("/state/procs");
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    write_file(f, out);
}

void Host::reap_nohang() {
    bool changed = false;
    for (auto& [pid, p] : procs_) {
        if (p.state != "running" || p.mode == "capture") continue;
        int st;
        if (::waitpid(pid, &st, WNOHANG) == pid) { p.state = "exited " + status_str(st); changed = true; }
    }
    if (changed) write_procs();
}

ToolResult Host::t_spawn(const Args& a) {
    const std::string& exe = need(a, "spawn", "exe");
    need_abs(exe);
    std::string argv_s = opt(a, "argv", "");
    std::string mode = opt(a, "mode", "capture");
    long long timeout = to_int(opt(a, "timeout_s", "60"), "timeout_s");
    long long mem_mb = to_int(opt(a, "mem_mb", "512"), "mem_mb");
    std::string in = opt(a, "stdin", "");
    if (mode != "capture" && mode != "background" && mode != "tty")
        throw ArgError{"spawn: mode must be capture, background or tty"};

    std::string path = resolve(exe);
    if (::access(path.c_str(), X_OK) != 0) return err("spawn: " + exe + ": " + std::strerror(errno));

    std::vector<std::string> argv = split_argv(argv_s);
    argv.insert(argv.begin(), exe);
    std::vector<char*> cargv;
    for (auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);

    int inpipe[2] = {-1, -1}, outpipe[2] = {-1, -1}, execpipe[2];
    if (mode == "capture") {
        if (::pipe2(inpipe, O_CLOEXEC) || ::pipe2(outpipe, O_CLOEXEC)) return err("spawn: pipe failed");
    }
    if (::pipe2(execpipe, O_CLOEXEC)) return err("spawn: pipe failed");

    if (mode == "background") {
        fs::create_directories(real("/state/log"));
    }
    int ttyfd = -1;
    if (mode == "tty") {
        ttyfd = ::open(cfg_.tty_path.c_str(), O_RDWR | O_CLOEXEC);
        if (ttyfd < 0) return err("spawn: tty " + cfg_.tty_path + ": " + std::strerror(errno));
    }

    // The child waits for the parent to set up its cgroup before exec.
    int gopipe[2];
    if (::pipe2(gopipe, O_CLOEXEC)) return err("spawn: pipe failed");

    pid_t pid = ::fork();
    if (pid < 0) return err("spawn: fork failed");
    if (pid == 0) {
        ::setpgid(0, 0);
        ::signal(SIGPIPE, SIG_DFL);
        ::signal(SIGINT, SIG_DFL);
        char go;
        ::close(gopipe[1]);
        if (::read(gopipe[0], &go, 1) != 1) ::_exit(127);
        if (mode == "capture") {
            ::dup2(inpipe[0], 0);
            ::dup2(outpipe[1], 1);
            ::dup2(outpipe[1], 2);
        } else if (mode == "background") {
            int nul = ::open("/dev/null", O_RDONLY);
            if (nul >= 0) ::dup2(nul, 0);
            std::string lp = real("/state/log/" + std::to_string(::getpid()));
            int fd = ::open(lp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { ::dup2(fd, 1); ::dup2(fd, 2); }
        } else {
            ::setsid();
            ::ioctl(ttyfd, TIOCSCTTY, 1);
            ::dup2(ttyfd, 0); ::dup2(ttyfd, 1); ::dup2(ttyfd, 2);
        }
        ::execv(path.c_str(), cargv.data());
        int e = errno;
        (void)!::write(execpipe[1], &e, sizeof e);
        ::_exit(127);
    }
    ::setpgid(pid, pid);
    ::close(gopipe[0]);
    ::close(execpipe[1]);
    if (inpipe[0] >= 0) { ::close(inpipe[0]); ::close(outpipe[1]); }
    if (ttyfd >= 0) ::close(ttyfd);

    if (!cfg_.cgroup_root.empty()) {
        fs::path cg = fs::path(cfg_.cgroup_root) / "potemkin" / std::to_string(pid);
        std::error_code ec;
        fs::create_directories(cg, ec);
        write_file(cg / "memory.max", std::to_string(mem_mb << 20));
        write_file(cg / "pids.max", "256");
        write_file(cg / "cgroup.procs", std::to_string(pid));
    }
    (void)!::write(gopipe[1], "g", 1);
    ::close(gopipe[1]);

    int eno = 0;
    ssize_t got = ::read(execpipe[0], &eno, sizeof eno);
    ::close(execpipe[0]);
    if (got == sizeof eno) {
        ::waitpid(pid, nullptr, 0);
        if (inpipe[1] >= 0) { ::close(inpipe[1]); ::close(outpipe[0]); }
        return err("spawn: exec " + exe + ": " + std::strerror(eno));
    }

    std::string argv_line = exe.substr(exe.rfind('/') + 1);
    if (!argv_s.empty()) argv_line += " " + argv_s;
    Proc p{pid, mode, argv_line, "running", turn_, snapshots_, ""};

    if (mode == "background") {
        p.log = "/state/log/" + std::to_string(pid);
        procs_[pid] = p;
        write_procs();
        return {"pid=" + std::to_string(pid) + " log=" + p.log};
    }

    procs_[pid] = p;
    write_procs();

    if (mode == "tty") {
        int st = 0;
        ::waitpid(pid, &st, 0);
        procs_[pid].state = "exited " + status_str(st);
        write_procs();
        return {status_str(st)};
    }

    // capture: feed stdin, collect output until exit or timeout.
    std::string out;
    size_t in_off = 0;
    if (in.empty()) { ::close(inpipe[1]); inpipe[1] = -1; }
    else ::fcntl(inpipe[1], F_SETFL, O_NONBLOCK);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    bool timed_out = false;
    const size_t keep = cfg_.max_result_bytes * 4;  // bound memory; cap() trims later
    while (outpipe[0] >= 0) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) { timed_out = true; break; }
        int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        struct pollfd fds[2] = {{outpipe[0], POLLIN, 0}, {inpipe[1], POLLOUT, 0}};
        int nf = inpipe[1] >= 0 ? 2 : 1;
        if (::poll(fds, nf, std::min(ms, 1000)) < 0 && errno != EINTR) break;
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char buf[65536];
            ssize_t r = ::read(outpipe[0], buf, sizeof buf);
            if (r <= 0) { ::close(outpipe[0]); outpipe[0] = -1; }
            else if (out.size() < keep) out.append(buf, std::min<size_t>(r, keep - out.size()));
        }
        if (nf == 2 && (fds[1].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = ::write(inpipe[1], in.data() + in_off, in.size() - in_off);
            if (w > 0) in_off += w;
            if (w < 0 || in_off >= in.size()) { ::close(inpipe[1]); inpipe[1] = -1; }
        }
    }
    if (inpipe[1] >= 0) ::close(inpipe[1]);
    if (outpipe[0] >= 0) ::close(outpipe[0]);
    int st = 0;
    if (timed_out) ::kill(-pid, SIGKILL);
    ::waitpid(pid, &st, 0);
    procs_[pid].state = "exited " + status_str(st);
    write_procs();
    procs_.erase(pid);  // capture children don't linger in the table
    write_procs();
    std::string head = timed_out ? "timeout after " + std::to_string(timeout) + "s, killed; " + status_str(st)
                                 : status_str(st);
    if (out.size() > cfg_.max_result_bytes) out = tail(out, cfg_.max_result_bytes - 64);
    return cap(head + "\n" + out);
}

ToolResult Host::t_wait(const Args& a) {
    pid_t pid = static_cast<pid_t>(to_int(need(a, "wait", "pid"), "pid"));
    std::string sig = opt(a, "signal", "");
    long long timeout = to_int(opt(a, "timeout_s", "60"), "timeout_s");
    auto it = procs_.find(pid);
    if (it == procs_.end()) return err("wait: no child with pid " + std::to_string(pid));
    Proc& p = it->second;
    if (!sig.empty() && p.state == "running") {
        int s = parse_signal(sig);
        if (::kill(-pid, s) != 0 && ::kill(pid, s) != 0)
            return err("wait: signal " + sig + ": " + std::strerror(errno));
    }
    if (p.state == "running") {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        int st;
        for (;;) {
            pid_t r = ::waitpid(pid, &st, WNOHANG);
            if (r == pid) { p.state = "exited " + status_str(st); break; }
            if (std::chrono::steady_clock::now() >= deadline) break;
            ::usleep(20000);
        }
        write_procs();
    }
    std::string out = p.state == "running" ? "still running" : p.state.substr(7);
    if (!p.log.empty()) {
        std::string log;
        read_all(real(p.log), log);
        out += "\n" + tail(log, 2048);
    }
    return cap(out);
}

// ---------------------------------------------------------------- compile

ToolResult Host::t_compile(const Args& a) {
    const std::string& lang = need(a, "compile", "lang");
    const std::string& name = need(a, "compile", "name");
    const std::string& source = need(a, "compile", "source");
    std::string opts = opt(a, "opts", "");
    if (lang != "c") return err("compile: only lang=c is supported");
    if (name.empty() || name.find('/') != std::string::npos || name == "." || name == "..")
        return err("compile: name must be a plain file name");
    if (cfg_.cc.empty()) return err("compile: no compiler configured");

    std::string hex = sha256_hex(lang + "\n" + opts + "\n" + source);
    std::string key = "sha256:" + hex;
    std::string sdir = "/store/" + key;
    fs::path dir = real(sdir);
    fs::path link = real("/generated/bin/" + name);
    auto do_link = [&] {
        std::error_code ec;
        fs::create_directories(link.parent_path(), ec);
        fs::remove(link, ec);
        fs::create_symlink(sdir + "/bin", link, ec);
    };

    if (fs::exists(dir / "bin")) {
        do_link();
        return {"exit=0 cached " + key + "\n/generated/bin/" + name + " -> " + sdir + "/bin"};
    }

    fs::path tmp = real("/store/.tmp-" + hex);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    write_file(tmp / "source.c", source);

    std::vector<std::string> argv;
    for (auto& s : cfg_.cc) argv.push_back(s == "@SRC@" ? (tmp / "source.c").string() : s);
    bool placed = std::find(cfg_.cc.begin(), cfg_.cc.end(), "@SRC@") != cfg_.cc.end();
    if (!placed) argv.push_back((tmp / "source.c").string());
    for (auto& o : split_argv(opts)) argv.insert(argv.begin() + 1, o);
    argv.push_back("-o");
    argv.push_back((tmp / "bin").string());

    int outp[2];
    if (::pipe2(outp, O_CLOEXEC)) return err("compile: pipe failed");
    pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(outp[1], 1);
        ::dup2(outp[1], 2);
        std::vector<char*> cv;
        for (auto& s : argv) cv.push_back(const_cast<char*>(s.c_str()));
        cv.push_back(nullptr);
        ::execv(cv[0], cv.data());
        ::_exit(127);
    }
    ::close(outp[1]);
    std::string diag;
    char buf[4096];
    ssize_t r;
    while ((r = ::read(outp[0], buf, sizeof buf)) > 0) diag.append(buf, r);
    ::close(outp[0]);
    int st = 0;
    ::waitpid(pid, &st, 0);
    // Scrub the temp path so the model sees its own file name, not ours.
    for (size_t pos; (pos = diag.find((tmp / "source.c").string())) != std::string::npos;)
        diag.replace(pos, (tmp / "source.c").string().size(), name + ".c");

    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || !fs::exists(tmp / "bin")) {
        fs::remove_all(tmp, ec);
        return cap(status_str(st) + "\n" + diag);
    }

    char ts[32];
    std::time_t now = std::time(nullptr);
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    std::string manifest = "{\n  \"hash\": " + json_str(key) + ",\n  \"name\": " + json_str(name) +
                           ",\n  \"lang\": " + json_str(lang) + ",\n  \"opts\": " + json_str(opts) +
                           ",\n  \"turn\": " + std::to_string(turn_) + ",\n  \"model\": " + json_str(cfg_.model_name) +
                           ",\n  \"timestamp\": " + json_str(ts) + "\n}\n";
    write_file(tmp / "manifest.json", manifest);
    fs::rename(tmp, dir, ec);
    if (ec && !fs::exists(dir / "bin")) return err("compile: store rename: " + ec.message());
    fs::remove_all(tmp, ec);
    do_link();
    std::string body = "exit=0 " + key + "\n/generated/bin/" + name + " -> " + sdir + "/bin";
    if (!diag.empty()) body += "\n" + diag;
    return cap(body);
}

// ---------------------------------------------------------------- snapshot

ToolResult Host::t_snapshot(const Args& a) {
    std::string rb = opt(a, "rollback", "");
    fs::path base = real(cfg_.snapshot_dir);
    if (rb.empty()) {
        int id = snapshots_ + 1;
        fs::path d = base / std::to_string(id);
        std::error_code ec;
        fs::remove_all(d, ec);
        copy_tree(real("/generated"), d / "generated");
        copy_tree(real("/state"), d / "state");
        snapshots_ = id;
        return {"snapshot=" + std::to_string(id)};
    }
    long long id = to_int(rb, "rollback");
    fs::path d = base / std::to_string(id);
    if (id < 1 || id > snapshots_ || !fs::exists(d)) return err("snapshot: no snapshot " + rb);

    // Children from a future that no longer exists go first.
    std::string killed;
    for (auto& [pid, p] : procs_) {
        if (p.snapshot_epoch >= id && p.state == "running") {
            ::kill(-pid, SIGKILL);
            int st;
            ::waitpid(pid, &st, 0);
            p.state = "killed by rollback";
            killed += " " + std::to_string(pid);
        }
    }
    std::error_code ec;
    for (const char* t : {"/generated", "/state"}) {
        fs::path live = real(t);
        if (fs::exists(live)) {
            for (auto& e : fs::directory_iterator(live, ec)) fs::remove_all(e.path(), ec);
        }
        fs::path snap = d / fs::path(t).filename();
        if (fs::exists(snap)) copy_tree(snap, live);
    }
    for (auto it = procs_.begin(); it != procs_.end();)
        it = it->second.state == "killed by rollback" ? procs_.erase(it) : std::next(it);
    // The restored table predates the rollback; rewrite it from what is alive now.
    write_procs();
    std::string out = "rolled back to snapshot=" + rb;
    if (!killed.empty()) out += "; killed" + killed;
    return {out};
}

// ---------------------------------------------------------------- fetch

ToolResult Host::t_fetch(const Args& a) {
    need(a, "fetch", "url");
    if (!cfg_.netboot) return err("fetch: not available offline");
    return err("fetch: not implemented yet");
}

}  // namespace pk
