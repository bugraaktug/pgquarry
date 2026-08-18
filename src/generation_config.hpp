#pragma once

#include <string>

namespace pgquarry
{

// Optional — unset model_path means "no generation capability configured";
// the worker still starts, but a claimed 'generate' job fails per-job rather
// than blocking startup the way a missing embedding model_path does.
struct GenerationConfig {
    std::string provider     = "llama"; // v1: only "llama" is implemented
    std::string model_path;             // generation_model_path; empty = disabled
    int         max_tokens   = 128;     // generation_max_tokens — fallback when a job's max_tokens is NULL
    int         n_ctx        = 2048;    // generation_n_ctx — must fit prompt tokens + max_tokens
    int         n_threads    = 4;
    int         n_gpu_layers = 0;
};

} // namespace pgquarry
