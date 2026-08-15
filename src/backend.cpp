#include "backend.h"

#include <string_view>

namespace persona {

#ifndef PERSONA_DEFAULT_BACKEND
#define PERSONA_DEFAULT_BACKEND "cpu"
#endif

// The flake sets PERSONA_DEFAULT_BACKEND per variant, so its value tells us
// which backend this binary was compiled with (a CPU-only build physically
// has no Vulkan kernels).
constexpr bool kHasVulkan = std::string_view(PERSONA_DEFAULT_BACKEND) == "vulkan";

const char* default_backend() { return PERSONA_DEFAULT_BACKEND; }

bool parse_backend(const std::string& name, engine::core::BackendType& out,
                   std::string& err) {
    if (name == "cpu") {
        out = engine::core::BackendType::Cpu;
        return true;
    }
    if (name == "vulkan") {
        if (!kHasVulkan) {
            err = "this binary was built without the Vulkan backend; build it "
                  "with:  nix build .#persona-vulkan";
            return false;
        }
        out = engine::core::BackendType::Vulkan;
        return true;
    }
    err = "unknown backend '" + name + "'; valid values: cpu" +
          (kHasVulkan ? ", vulkan" : "");
    return false;
}

}  // namespace persona
