// In-process q27 backend (llm=cuda). The only translation unit that sees CUDA.
#pragma once
#include <memory>
#include <string>

#include "../harness/harness.h"

namespace pk {

struct Q27Opts {
    std::string model, tok;
    int ctx = -1;                 // -1: size from free VRAM
    double fixed_stack_gb = 0.6;  // non-KV engine stack (12g build + slim pack measure ~0.54)
    std::string prefix_cache;     // dir, empty = off
    float temp = 1.0f, top_p = 0.95f, min_p = 0.05f;
    int top_k = 20;
    bool think = true;
    int n_max = 16384;            // per assistant round
    bool verbose = false;         // q27's own stderr chatter
};

std::unique_ptr<Backend> make_q27_backend(const Q27Opts& o);

}  // namespace pk
