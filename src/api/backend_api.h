// llm=api: an OpenAI-compatible chat/completions endpoint (OpenAI, vLLM,
// llama.cpp, OpenRouter, or q27-server on another box). Streams over SSE.
#pragma once
#include <memory>
#include <string>

#include "../harness/harness.h"

namespace pk {

struct ApiOpts {
    std::string url = "https://api.openai.com/v1";  // base; /chat/completions is appended
    std::string key;                                // Bearer token; may be empty for local servers
    std::string model = "gpt-4.1";
    int context = 128000;
    int max_tokens = 32768;  // per reply; servers default lower (q27-server: 8192)
    int timeout_s = 600;
    std::string ca_file = "/etc/ssl/certs/ca-certificates.crt";
};

std::unique_ptr<Backend> make_api_backend(const ApiOpts& o);

}  // namespace pk

namespace pk {
// GET a URL for the fetch tool (netboot). Follows redirects; err set on failure.
std::string http_fetch(const std::string& url, std::string& err, const std::string& ca_file);
}  // namespace pk
