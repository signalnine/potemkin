// In-process q27 backend (llm=cuda). The only translation unit that sees CUDA.
#pragma once
#include <memory>
#include <string>

#include "../harness/harness.h"

namespace pk {

struct Q27Opts {
    std::string model, tok;
    int ctx = -1;                 // -1: size from free VRAM
    double fixed_stack_gb = -1;   // non-KV engine stack; <0: the server's per-arch calibration
                                  // (the 12g build + slim pack measure ~0.54: pass 0.6)
    std::string dflash2;          // DFlash2 serving pack (.d2w), empty = MTP only
    std::string prefix_cache;     // dir, empty = off
    float temp = 1.0f, top_p = 0.95f, min_p = 0.05f;
    int top_k = 20;
    bool think = true;
    int think_budget = -1;        // >0: force </think> after this many thinking tokens (q27's
                                  // --think-budget); off by default, the measured recipe is unbounded
    int n_max = 65536;            // per assistant round, clamped to the window; a shell takes >16K of planning
    bool verbose = false;         // q27's own stderr chatter
};

std::unique_ptr<Backend> make_q27_backend(const Q27Opts& o);

}  // namespace pk
