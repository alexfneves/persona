#pragma once

// Backend selection for persona. The backend is a BUILD-TIME property of the
// binary (audio.cpp/ggml compiles backend kernels into the static library, so
// a CPU-only build physically has no Vulkan kernels). The chosen backend is
// compiled in as the default via -DPERSONA_DEFAULT_BACKEND (see flake.nix);
// --backend only overrides it (e.g. forcing CPU on a Vulkan build).

#include <string>

#include "engine/framework/core/module.h"  // engine::core::BackendType

namespace persona {

// The backend compiled into this binary ("cpu" unless the flake defined
// -DPERSONA_DEFAULT_BACKEND, e.g. "vulkan" for .#persona-vulkan).
const char* default_backend();

// Maps a user-facing backend name ("cpu", "vulkan") to the engine enum.
// Returns false and fills `err` for unknown names.
bool parse_backend(const std::string& name, engine::core::BackendType& out,
                   std::string& err);

// Human-readable name of a backend type ("cpu", "vulkan", ...). Unlike
// default_backend() — which reports the compiled-in DEFAULT — this names the
// type actually in use, so per-session debug lines can show the real backend.
const char* backend_name(engine::core::BackendType t);

}  // namespace persona
