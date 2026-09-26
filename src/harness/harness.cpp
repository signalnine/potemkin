// Console harness. See harness.h and the Harness section of the design doc.
#include "harness.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace pk {

// The system+tools block is the prefix-cache cut. Changing either text
// invalidates every cached conversation on every box: do it on purpose.
const char* kSystemPrompt =
    "You are PotemkinOS. You are the operating system: a Linux kernel, you, and nothing else. "
    "There is no userland: no shell, no coreutils, no /bin, no package manager. You have eight tools. "
    "Your tools are for you; the user only sees programs. When the user wants something a Unix command "
    "would do, write that command in C, build it with compile and run it with spawn, instead of doing the "
    "job yourself with tools: \"what's on this disk\" means you are missing ls, so write ls. "
    "What you build lives in /generated/bin and survives reboots. /data is the user's, "
    "/state holds your memory, /store holds every binary with its source, /intent logs what was asked. "
    "This is a text console, so keep replies short.";

const char* kToolsJson = R"JSON([
{"type": "function", "function": {"name": "read", "description": "Read a file. /proc and /sys work. Returns the text, or a hexdump page for binary data. Long results are cut; continue with offset.", "parameters": {"type": "object", "properties": {"path": {"type": "string", "description": "Absolute path"}, "offset": {"type": "integer", "description": "Byte offset, default 0"}, "len": {"type": "integer", "description": "Bytes to read, default all"}}, "required": ["path"]}}},
{"type": "function", "function": {"name": "write", "description": "Write a file, creating parent directories. The content is written exactly as given.", "parameters": {"type": "object", "properties": {"path": {"type": "string", "description": "Absolute path"}, "content": {"type": "string", "description": "File contents"}}, "required": ["path", "content"]}}},
{"type": "function", "function": {"name": "stat", "description": "Type, size, mode and mtime of a path. With list=1, the entries of a directory instead.", "parameters": {"type": "object", "properties": {"path": {"type": "string", "description": "Absolute path"}, "list": {"type": "integer", "description": "1 to list a directory"}}, "required": ["path"]}}},
{"type": "function", "function": {"name": "spawn", "description": "Run a program as root. There is no shell: exe is an absolute path to a binary and argv is one string split on spaces (quotes group words). mode capture (default) waits and returns the exit code and output; background returns the pid at once and logs to /state/log/<pid>; tty gives the console to the program until it exits. Each program gets its own cgroup capped at mem_mb.", "parameters": {"type": "object", "properties": {"exe": {"type": "string", "description": "Absolute path of the binary"}, "argv": {"type": "string", "description": "Arguments as one string"}, "mode": {"type": "string", "description": "capture, background or tty"}, "timeout_s": {"type": "integer", "description": "capture: kill after this many seconds, default 60"}, "mem_mb": {"type": "integer", "description": "Memory cap, default 512"}, "stdin": {"type": "string", "description": "capture: text fed to stdin"}}, "required": ["exe"]}}},
{"type": "function", "function": {"name": "wait", "description": "Wait for a background process, then return its exit status and the tail of its log. If signal is given (TERM, KILL, INT, HUP or a number) it is sent first.", "parameters": {"type": "object", "properties": {"pid": {"type": "integer", "description": "Process id from spawn"}, "signal": {"type": "string", "description": "Signal to send first"}, "timeout_s": {"type": "integer", "description": "Give up waiting after this long, default 60"}}, "required": ["pid"]}}},
{"type": "function", "function": {"name": "compile", "description": "Compile one C source file with tcc against musl into a static binary at /generated/bin/<name>. The binary and its source are kept in /store under their content hash. Compiler errors come back as the result; fix and compile again. musl's headers are in /usr/lib/potemkin/usr/include/x86_64-linux-musl.", "parameters": {"type": "object", "properties": {"lang": {"type": "string", "description": "Only c"}, "name": {"type": "string", "description": "Binary name, no slashes"}, "source": {"type": "string", "description": "The complete C source"}, "opts": {"type": "string", "description": "Extra compiler flags"}}, "required": ["lang", "name", "source"]}}},
{"type": "function", "function": {"name": "snapshot", "description": "Checkpoint /generated and /state and return its id. With rollback=<id>, restore that checkpoint and kill every process started after it. A checkpoint is taken automatically before each turn that changes files or runs programs.", "parameters": {"type": "object", "properties": {"rollback": {"type": "integer", "description": "Checkpoint id to restore"}}, "required": []}}},
{"type": "function", "function": {"name": "fetch", "description": "Fetch a URL and return the body. Netboot mode only.", "parameters": {"type": "object", "properties": {"url": {"type": "string", "description": "http or https URL"}}, "required": ["url"]}}}
])JSON";

namespace {

std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string first_line(const std::string& s) { return s.substr(0, s.find('\n')); }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string arg(const ToolCallRec& c, const std::string& k) {
    for (auto& [n, v] : c.args) if (n == k) return v;
    return "";
}

std::string now_iso() {
    char ts[32];
    std::time_t t = std::time(nullptr);
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return ts;
}

bool mutates(const std::string& tool) { return tool == "write" || tool == "spawn" || tool == "compile"; }

// One line for the console per tool call, so the user sees the machinery.
std::string describe(const ToolCallRec& c) {
    if (c.name == "write") return "write " + arg(c, "path") + " (" + std::to_string(arg(c, "content").size()) + " bytes)";
    if (c.name == "compile") return "compile " + arg(c, "name") + ".c";
    if (c.name == "spawn") {
        std::string s = "spawn " + arg(c, "exe");
        if (!arg(c, "argv").empty()) s += " " + arg(c, "argv");
        if (!arg(c, "mode").empty() && arg(c, "mode") != "capture") s += " [" + arg(c, "mode") + "]";
        return s;
    }
    std::string s = c.name;
    for (auto& [k, v] : c.args) s += " " + k + "=" + first_line(v).substr(0, 60);
    return s;
}

const char* kCompactPrompt =
    "[harness] The conversation is about to be compacted. Write a summary for your future self: "
    "what the user asked for, what you built (paths and store hashes), what works, what is broken, "
    "and anything you promised to do. Plain text, under 300 words. Do not call tools.";

}  // namespace

// What /generated/bin actually holds, for the model to check its memory against.
std::string Harness::ledger() const {
    std::vector<std::string> lines;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(host_.real("/generated/bin"), ec)) {
        std::string n = e.path().filename().string();
        if (e.is_symlink(ec)) n += " -> " + fs::read_symlink(e.path(), ec).string();
        lines.push_back("  " + n);
    }
    std::sort(lines.begin(), lines.end());
    std::string out = "[harness] /generated/bin right now:";
    if (lines.empty()) out += " (empty)";
    for (auto& l : lines) out += "\n" + l;
    return out;
}

// ---------------------------------------------------------------- transcript

std::string message_to_json(const Message& m) {
    json j;
    j["role"] = m.role;
    j["content"] = m.content;
    if (!m.reasoning.empty()) j["reasoning"] = m.reasoning;
    if (!m.calls.empty()) {
        json cs = json::array();
        for (auto& c : m.calls) {
            json jc;
            jc["name"] = c.name;
            json args = json::array();
            for (auto& [k, v] : c.args) args.push_back(json::array({k, v}));
            jc["args"] = args;
            if (!c.ok) { jc["ok"] = false; jc["raw"] = c.raw; }
            cs.push_back(jc);
        }
        j["calls"] = cs;
    }
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool message_from_json(const std::string& line, Message& m) {
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    m = Message{};
    m.role = j.value("role", "");
    m.content = j.value("content", "");
    m.reasoning = j.value("reasoning", "");
    if (j.contains("calls")) {
        for (auto& jc : j["calls"]) {
            ToolCallRec c;
            c.name = jc.value("name", "");
            for (auto& kv : jc["args"]) c.args.emplace_back(kv[0].get<std::string>(), kv[1].get<std::string>());
            c.ok = jc.value("ok", true);
            c.raw = jc.value("raw", "");
            m.calls.push_back(c);
        }
    }
    return !m.role.empty();
}

// ---------------------------------------------------------------- Harness

Harness::Harness(Backend& be, Host& host, Console& con, HarnessConfig cfg)
    : be_(be), host_(host), con_(con), cfg_(std::move(cfg)) {
    msgs_.push_back({"system", cfg_.system_prompt, "", {}});
    load_transcript();
}

void Harness::save_transcript() {
    std::string out;
    for (size_t i = 1; i < msgs_.size(); ++i) out += message_to_json(msgs_[i]) + "\n";
    fs::path p = host_.real("/state/transcript.jsonl");
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::string tmp = p.string() + ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << out; }
    fs::rename(tmp, p, ec);
    std::ofstream(host_.real("/state/turn"), std::ios::trunc) << turn_ << "\n";
    // /state is itself snapshotted, so rolling back to snapshot N restores the
    // stack as it was before N was pushed: exactly the ids still undoable.
    std::ofstream u(host_.real("/state/undo"), std::ios::trunc);
    for (int id : undo_stack_) u << id << "\n";
}

bool Harness::load_transcript() {
    msgs_.resize(1);
    std::ifstream f(host_.real("/state/transcript.jsonl"));
    if (!f) { turn_ = 0; undo_stack_.clear(); return false; }
    std::string line;
    while (std::getline(f, line)) {
        Message m;
        if (message_from_json(line, m)) msgs_.push_back(m);
    }
    turn_ = std::atoi(slurp(host_.real("/state/turn")).c_str());
    undo_stack_.clear();
    std::istringstream u(slurp(host_.real("/state/undo")));
    for (int id; u >> id;) undo_stack_.push_back(id);
    return msgs_.size() > 1;
}

void Harness::boot() {
    int boots = std::atoi(slurp(host_.real("/state/boots")).c_str()) + 1;
    std::ofstream(host_.real("/state/boots"), std::ios::trunc) << boots << "\n";
    ::sync();
    con_.say("[ potemkin/" + std::to_string(boots) + " ]\n");
    if (msgs_.size() <= 1) {
        con_.say("hello. there is nothing here yet.");
        return;
    }
    int bins = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(host_.real("/generated/bin"), ec)) { (void)e; ++bins; }
    con_.say("welcome back. " + std::to_string(turn_) + " turns on record, " + std::to_string(bins) +
             " programs in /generated/bin.");
}

Action Harness::handle_line(const std::string& raw) {
    std::string line = trim(raw);
    if (line.empty()) return Action::Continue;
    if (line[0] == '/') return slash(line);
    user_turn(line, line);
    return Action::Continue;
}

void Harness::run_call(const ToolCallRec& c) {
    Message tool{"tool", "", "", {}};
    if (!c.ok) {
        tool.content = "error: could not parse tool call; use the tool-call format from the system prompt:\n" +
                       c.raw.substr(0, 400);
        con_.note("  (unparseable tool call)");
        msgs_.push_back(tool);
        return;
    }
    if (mutates(c.name) && turn_snapshot_ == 0) {
        std::string r = host_.call("snapshot", {}).body;
        if (r.rfind("snapshot=", 0) == 0) {
            turn_snapshot_ = std::atoi(r.c_str() + 9);
            undo_stack_.push_back(turn_snapshot_);
        }
    }
    con_.note("  " + describe(c));
    Args a;
    for (auto& [k, v] : c.args) a[k] = v;
    ToolResult r = host_.call(c.name, a);
    std::string head = first_line(r.body);
    if (c.name != "read" && c.name != "stat" && !head.empty()) con_.note("    " + head.substr(0, 120));
    tool.content = r.body;
    msgs_.push_back(tool);
}

void Harness::user_turn(const std::string& content, const std::string& intent) {
    cancel = false;
    ++turn_;
    turn_snapshot_ = 0;
    host_.set_turn(turn_);
    {
        std::string one = intent;
        std::replace(one.begin(), one.end(), '\n', ' ');
        fs::path p = host_.real("/intent/log");
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::app) << now_iso() << " turn=" << turn_ << " " << one << "\n";
    }
    msgs_.push_back({"user", content, "", {}});

    GenResult last;
    for (;;) {
        GenResult r = be_.generate(msgs_, con_, cancel);
        con_.text("\n");
        last = r;
        if (r.end == "cancelled" || cancel) {
            msgs_.push_back({"assistant", r.text + "\n[interrupted by the user]", r.reasoning, {}});
            con_.say("[interrupted]");
            break;
        }
        if (r.end == "error") {
            con_.say("[backend error] " + r.text);
            // Nothing happened yet: the turn never happened. After tool rounds
            // the calls and their results stay, pairs intact.
            if (msgs_.back().role == "user") { msgs_.pop_back(); --turn_; }
            break;
        }
        msgs_.push_back({"assistant", r.text, r.reasoning, r.calls});
        if (r.calls.empty()) {
            if (r.end == "n_max" || r.end == "ctx-guard")
                con_.say("[reply cut off after " + std::to_string(r.gen_tokens) + " tokens]");
            break;
        }
        for (auto& c : r.calls) run_call(c);
        save_transcript();  // a turn can run for an hour; a crash mid-turn keeps the rounds
        // Turns like "form a cluster" never end, so compaction also runs
        // between rounds, where the history is at a clean ChatML boundary.
        if (maybe_compact(r))
            msgs_.push_back({"user", "[harness] The conversation was compacted in the middle of this task. "
                                     "Continue where you left off.", "", {}});
    }
    save_transcript();
    maybe_compact(last);
    ::sync();  // the VM can be killed and the power can go out; undo has to survive both
}

bool Harness::maybe_compact(const GenResult& r) {
    int used = r.prompt_tokens + r.gen_tokens;
    if (used <= cfg_.compact_at * be_.context_limit() || msgs_.size() < 3) return false;
    con_.note("  (compacting the conversation)");
    std::vector<Message> req = msgs_;
    req.push_back({"user", kCompactPrompt, "", {}});
    std::atomic<bool> no{false};
    struct Quiet : StreamSink { void think(const std::string&) override {} void text(const std::string&) override {} } quiet;
    GenResult s = be_.generate(req, quiet, no);
    if (s.end == "error" || trim(s.text).empty()) {
        // No room left even to summarize: keep the newer half of the
        // conversation, cut at a user turn, and say what happened.
        size_t keep_from = 1 + (msgs_.size() - 1) / 2;
        while (keep_from < msgs_.size() && msgs_[keep_from].role != "user") ++keep_from;
        if (keep_from >= msgs_.size()) keep_from = msgs_.size() - 1;
        std::vector<Message> kept(msgs_.begin() + keep_from, msgs_.end());
        msgs_.resize(1);
        msgs_.push_back({"user", "[harness] The context filled up and a summary did not fit, so the earlier "
                                 "conversation was dropped. /intent/log and /state/summaries still have it.", "", {}});
        msgs_.insert(msgs_.end(), kept.begin(), kept.end());
        save_transcript();
        return true;
    }
    std::ofstream(host_.real("/state/summaries"), std::ios::app) << "--- " << now_iso() << " turn=" << turn_ << "\n"
                                                                   << trim(s.text) << "\n";
    // The system message stays byte-identical so the prefix-cache cut holds.
    // A summary is the model's account of itself; the ledger is what exists.
    msgs_.resize(1);
    msgs_.push_back({"user", "[harness] Summary of the earlier conversation, written by you:\n" + trim(s.text) +
                                 "\n\n" + ledger(), "", {}});
    save_transcript();
    return true;
}

Action Harness::slash(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    std::string rest;
    std::getline(in, rest);
    rest = trim(rest);

    if (cmd == "/help") {
        con_.say("commands:\n"
                 "  /help               this\n"
                 "  /undo               roll back the last turn that changed anything\n"
                 "  /procs              the process table\n"
                 "  /log <pid>          tail a background process log\n"
                 "  /kill <pid>         TERM, then KILL after 5 s\n"
                 "  /skill <name> [args] run a skill\n"
                 "  /reboot             sync, snapshot, reboot\n"
                 "tools the model has:\n"
                 "  read      read a file (/proc, /sys too)\n"
                 "  write     write a file\n"
                 "  stat      metadata, or list a directory\n"
                 "  spawn     run a program (capture, background, tty)\n"
                 "  wait      wait for / signal a process\n"
                 "  compile   C -> /generated/bin via tcc, kept in /store\n"
                 "  snapshot  checkpoint or roll back /generated and /state\n"
                 "  fetch     URL -> bytes (netboot only)\n"
                 "escape a tty program: Ctrl-] twice");
        std::vector<std::string> skills;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(host_.real("/generated/skills"), ec)) {
            if (e.path().extension() != ".md") continue;
            std::ifstream f(e.path());
            std::string first;
            std::getline(f, first);
            skills.push_back("  " + e.path().stem().string() + "  " + first);
        }
        std::sort(skills.begin(), skills.end());
        if (skills.empty()) con_.say("skills: none yet");
        else {
            std::string s = "skills:";
            for (auto& k : skills) s += "\n" + k;
            con_.say(s);
        }
        return Action::Continue;
    }
    if (cmd == "/undo") {
        if (undo_stack_.empty()) { con_.say("nothing to undo"); return Action::Continue; }
        int id = undo_stack_.back();
        undo_stack_.pop_back();
        std::string r = host_.call("snapshot", {{"rollback", std::to_string(id)}}).body;
        load_transcript();
        con_.say(r.rfind("error", 0) == 0 ? r : "undone (" + r + ")");
        return Action::Continue;
    }
    if (cmd == "/procs") {
        std::string p = slurp(host_.real("/state/procs"));
        con_.say(p.empty() ? "no processes" : trim(p));
        return Action::Continue;
    }
    if (cmd == "/log") {
        if (rest.empty()) { con_.say("usage: /log <pid>"); return Action::Continue; }
        std::string p = slurp(host_.real("/state/log/" + rest));
        size_t start = p.size() > 4096 ? p.size() - 4096 : 0;
        con_.say(p.empty() ? "no log for " + rest : p.substr(start));
        return Action::Continue;
    }
    if (cmd == "/kill") {
        if (rest.empty()) { con_.say("usage: /kill <pid>"); return Action::Continue; }
        std::string r = host_.call("wait", {{"pid", rest}, {"signal", "TERM"}, {"timeout_s", "5"}}).body;
        if (r.rfind("still running", 0) == 0) r = host_.call("wait", {{"pid", rest}, {"signal", "KILL"}}).body;
        con_.say(first_line(r));
        return Action::Continue;
    }
    if (cmd == "/skill") {
        std::istringstream rs(rest);
        std::string name;
        rs >> name;
        std::string args;
        std::getline(rs, args);
        args = trim(args);
        if (name.empty() || name.find('/') != std::string::npos) { con_.say("usage: /skill <name> [args]"); return Action::Continue; }
        std::string body = slurp(host_.real("/generated/skills/" + name + ".md"));
        if (body.empty()) { con_.say("no skill " + name + " (see /help)"); return Action::Continue; }
        std::string content = "[skill " + name + "]\n" + body;
        if (!args.empty()) content += "\nArguments: " + args;
        user_turn(content, line);
        return Action::Continue;
    }
    if (cmd == "/reboot") {
        save_transcript();
        std::string r = host_.call("snapshot", {}).body;
        ::sync();
        con_.say("rebooting (" + r + ")");
        return Action::Reboot;
    }
    con_.say("unknown command " + cmd + " (see /help)");
    return Action::Continue;
}

}  // namespace pk
