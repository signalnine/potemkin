// The q27 backend for a q27-init built without CUDA (bash tools/build.sh api).
// Such a build speaks llm=api only.
#include "backend_q27.h"

#include <stdexcept>

namespace pk {

std::unique_ptr<Backend> make_q27_backend(const Q27Opts&) {
    throw std::runtime_error("this q27-init was built without CUDA (tools/build.sh api); boot with llm=api");
}

}  // namespace pk
