// Host primitives: the eight tools the model gets. See CONTRACT.md.
#pragma once
#include <map>
#include <string>
#include <vector>
#include <sys/types.h>

namespace pk {

using Args = std::map<std::string, std::string>;

struct ToolResult {
    std::string body;
    bool truncated = false;
};

struct Config {
    std::string root;                  // prefix for every model path ("" in the image)
    size_t max_result_bytes = 16384;   // ~4K tokens
    std::string cgroup_root;           // e.g. /sys/fs/cgroup; empty = no cgroups
    std::string tty_path = "/dev/tty1";
    std::string snapshot_dir = "/snapshots";  // model path, outside /generated and /state
    std::vector<std::string> cc;       // compiler argv prefix; source file + "-o out" appended
    std::string model_name = "unknown";
    bool netboot = false;
    bool isolate = false;              // dev: children chroot into root via user+mount namespaces
};

struct Proc {
    pid_t pid;
    std::string mode, argv, state;
    int turn;
    int snapshot_epoch;                // snapshot count when spawned
    std::string log;                   // model path of log (background)
};

class Host {
public:
    explicit Host(Config cfg);
    ~Host();
    ToolResult call(const std::string& name, const Args& args);
    std::vector<std::string> tool_names() const;
    void set_turn(int turn) { turn_ = turn; }
    std::string real(const std::string& model_path) const;     // root + path
    std::string resolve(const std::string& model_path) const;  // real(), following symlinks inside root

private:
    ToolResult t_read(const Args&);
    ToolResult t_write(const Args&);
    ToolResult t_stat(const Args&);
    ToolResult t_spawn(const Args&);
    ToolResult t_wait(const Args&);
    ToolResult t_compile(const Args&);
    ToolResult t_snapshot(const Args&);
    ToolResult t_fetch(const Args&);

    ToolResult cap(std::string body) const;
    void write_procs();
    void reap_nohang();

    Config cfg_;
    int turn_ = 0;
    int snapshots_ = 0;
    std::map<pid_t, Proc> procs_;
};

// Exposed for tests.
std::string sha256_hex(const std::string& data);
std::vector<std::string> split_argv(const std::string& s);

}  // namespace pk
