// The console harness: slash commands, the agent loop, transcript, ledger.
// Model-agnostic: generation goes through Backend, output through Console.
#pragma once
#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "../host/host.h"

namespace pk {

struct ToolCallRec {
    std::string name;
    std::vector<std::pair<std::string, std::string>> args;  // model's order
    bool ok = true;       // false: the parser could not make sense of it
    std::string raw;      // the model's text for a failed call
};

struct Message {
    std::string role;  // system | user | assistant | tool
    std::string content;
    std::string reasoning;              // assistant only
    std::vector<ToolCallRec> calls;     // assistant only
};

struct GenResult {
    std::string text, reasoning;
    std::vector<ToolCallRec> calls;
    std::string end;  // eos | n_max | ctx-guard | cancelled | error
    int prompt_tokens = 0, gen_tokens = 0;
};

// Live output while the model decodes.
struct StreamSink {
    virtual ~StreamSink() = default;
    virtual void think(const std::string& s) = 0;
    virtual void text(const std::string& s) = 0;
};

struct Backend {
    virtual ~Backend() = default;
    virtual GenResult generate(const std::vector<Message>& msgs, StreamSink& sink,
                               const std::atomic<bool>& cancel) = 0;
    virtual int context_limit() const = 0;
    virtual std::string name() const = 0;
};

struct Console : StreamSink {
    virtual void say(const std::string& s) = 0;   // harness messages
    virtual void note(const std::string& s) = 0;  // tool activity, dim
};

enum class Action { Continue, Reboot };

struct HarnessConfig {
    std::string system_prompt;
    double compact_at = 0.75;  // fraction of context_limit that triggers compaction
};

class Harness {
public:
    Harness(Backend& be, Host& host, Console& con, HarnessConfig cfg);
    void boot();                               // restore transcript, print banner
    Action handle_line(const std::string& line);
    std::atomic<bool> cancel{false};
    const std::vector<Message>& messages() const { return msgs_; }

private:
    void user_turn(const std::string& content, const std::string& intent);
    void run_call(const ToolCallRec& c);
    void save_transcript();
    bool load_transcript();
    bool maybe_compact(const GenResult& r);  // true if the history was replaced
    Action slash(const std::string& line);
    std::string ledger() const;

    Backend& be_;
    Host& host_;
    Console& con_;
    HarnessConfig cfg_;
    std::vector<Message> msgs_;
    int turn_ = 0;
    int turn_snapshot_ = 0;              // snapshot id taken this turn, 0 = none yet
    std::vector<int> undo_stack_;        // snapshot ids, one per mutating turn
};

// Frozen tool block: OpenAI-shaped, client-ordered JSON text.
extern const char* kToolsJson;
extern const char* kSystemPrompt;

// Transcript (de)serialization, one JSON object per line.
std::string message_to_json(const Message& m);
bool message_from_json(const std::string& line, Message& m);

}  // namespace pk
