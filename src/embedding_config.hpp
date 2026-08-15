#pragma once

#include <string>

namespace pgquarry 
{

// v0: populated from CLI flags in worker_main.cpp, not a TOML file (that's
// v1's pgquarry.toml). Trimmed to just the fields LlamaProvider reads, plus
// dimensions for a startup sanity check — see walkrie's config.hpp for the
// full EmbeddingConfig this was cut down from.
struct EmbeddingConfig {
    std::string provider       = "llama"; // v0: only "llama" is implemented
    std::string model_path;
    int         dimensions     = 1024;    // must match the model's actual output width
    int         n_threads      = 4;
    int         n_ctx          = 512;
    int         max_batch_size = 8;
    int         n_gpu_layers   = 0;
};

} // namespace pgquarry
