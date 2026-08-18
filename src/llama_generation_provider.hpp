#pragma once

#include <ggml.h>
#include <llama.h>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>

#include "generation_provider.hpp"

namespace pgquarry {

struct GenerationConfig;

class LlamaGenerationProvider : public GenerationProvider
{
public:
    explicit LlamaGenerationProvider(const GenerationConfig& cfg);
    ~LlamaGenerationProvider() override;

    LlamaGenerationProvider(const LlamaGenerationProvider&) = delete;
    LlamaGenerationProvider& operator=(const LlamaGenerationProvider&) = delete;

    void init() override;
    std::string generate(const std::string& prompt, int max_tokens) override;
    std::string name() const override;

private:
    std::string model_path_;
    int         n_threads_;
    int         n_ctx_;
    int         n_gpu_layers_;

    llama_model*        model_     = nullptr;
    llama_context*      ctx_       = nullptr;
    const llama_vocab*  vocab_     = nullptr;
    const char*         chat_tmpl_ = nullptr; // owned by model_, not freed separately

    std::mutex ctx_mutex_;
};

} // namespace pgquarry
