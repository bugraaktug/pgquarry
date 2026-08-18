// Generation-loop shape grounded in third_party/llama.cpp/examples/simple-chat/simple-chat.cpp,
// adapted to be single-shot/stateless (a fresh KV cache per call, no
// multi-turn message history) and thread-safe (mutex around the whole call).
#include "llama_generation_provider.hpp"
#include "generation_config.hpp"

#include <memory>
#include <stdexcept>
#include <vector>

namespace pgquarry {

LlamaGenerationProvider::LlamaGenerationProvider(const GenerationConfig& cfg)
    : model_path_(cfg.model_path)
    , n_threads_(cfg.n_threads)
    , n_ctx_(cfg.n_ctx)
    , n_gpu_layers_(cfg.n_gpu_layers) {}

LlamaGenerationProvider::~LlamaGenerationProvider()
{
    if (ctx_)   llama_free(ctx_);
    if (model_) llama_model_free(model_);
    // llama_backend_free() intentionally NOT called here — llama.cpp documents
    // backend init/free as call-once-per-process; worker_main.cpp's
    // LlamaBackendGuard owns that for the whole process, shared with LlamaProvider.
}

void LlamaGenerationProvider::init()
{
    llama_log_set([](ggml_log_level level, const char* text, void* /*user_data*/) {
        switch (level) {
            case GGML_LOG_LEVEL_ERROR:
                spdlog::error("[llama] {}", text); break;
            case GGML_LOG_LEVEL_WARN:
                spdlog::warn("[llama] {}", text);  break;
            default:
                spdlog::debug("[llama] {}", text); break;
        }
    }, nullptr);

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = n_gpu_layers_;
    if (n_gpu_layers_ > 0 && !llama_supports_gpu_offload()) {
        spdlog::warn("[LlamaGenerationProvider] n_gpu_layers={} requested but this build has no GPU backend — "
                     "falling back to CPU-only.", n_gpu_layers_);
        mparams.n_gpu_layers = 0;
    }

    model_ = llama_model_load_from_file(model_path_.c_str(), mparams);
    if (!model_) {
        throw std::runtime_error(
            "LlamaGenerationProvider: failed to load model from '" + model_path_ + "' — "
            "check that the path exists and is a valid GGUF file");
    }
    vocab_ = llama_model_get_vocab(model_);

    chat_tmpl_ = llama_model_chat_template(model_, /*name=*/nullptr);
    if (!chat_tmpl_) {
        llama_model_free(model_);
        model_ = nullptr;
        throw std::runtime_error(
            "LlamaGenerationProvider: model '" + model_path_ + "' has no built-in chat template — "
            "pgquarry only supports models with a baked-in chat_template (see GGUF metadata); "
            "pick an instruct/chat-tuned GGUF export instead");
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx           = static_cast<uint32_t>(n_ctx_);
    cparams.n_batch         = static_cast<uint32_t>(n_ctx_);
    cparams.n_threads       = static_cast<uint32_t>(n_threads_);
    cparams.n_threads_batch = static_cast<uint32_t>(n_threads_);
    // embeddings left false (default) — this is a generation-only context.

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_) {
        llama_model_free(model_);
        model_ = nullptr;
        throw std::runtime_error("LlamaGenerationProvider: failed to create llama context");
    }

    spdlog::info("[LlamaGenerationProvider] loaded {} (n_ctx={})", model_path_, n_ctx_);
}

std::string LlamaGenerationProvider::generate(const std::string& prompt, int max_tokens)
{
    std::lock_guard<std::mutex> lock(ctx_mutex_);

    // Stateless per call — no cross-call conversation memory.
    llama_memory_clear(llama_get_memory(ctx_), true);

    llama_chat_message msg{"user", prompt.c_str()};
    std::vector<char> formatted(prompt.size() * 2 + 256);
    int32_t formatted_len = llama_chat_apply_template(
        chat_tmpl_, &msg, 1, /*add_ass=*/true, formatted.data(), static_cast<int32_t>(formatted.size()));
    if (formatted_len > static_cast<int32_t>(formatted.size())) {
        formatted.resize(static_cast<size_t>(formatted_len));
        formatted_len = llama_chat_apply_template(
            chat_tmpl_, &msg, 1, true, formatted.data(), static_cast<int32_t>(formatted.size()));
    }
    if (formatted_len < 0) {
        throw std::runtime_error("LlamaGenerationProvider: failed to apply chat template");
    }
    const std::string templated_prompt(formatted.data(), static_cast<size_t>(formatted_len));

    // parse_special=true: the templated prompt embeds real special role/turn
    // tokens (e.g. <|im_start|>) that must tokenize as tokens, not literal
    // text — unlike LlamaProvider's plain embedding input.
    int n_prompt_tokens = -llama_tokenize(vocab_, templated_prompt.c_str(),
                                           static_cast<int32_t>(templated_prompt.size()),
                                           nullptr, 0, /*add_special=*/true, /*parse_special=*/true);
    std::vector<llama_token> prompt_tokens(static_cast<size_t>(n_prompt_tokens));
    if (llama_tokenize(vocab_, templated_prompt.c_str(), static_cast<int32_t>(templated_prompt.size()),
                        prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()),
                        true, true) < 0) {
        throw std::runtime_error("LlamaGenerationProvider: failed to tokenize prompt");
    }

    if (static_cast<int>(prompt_tokens.size()) + max_tokens > n_ctx_) {
        throw std::runtime_error(
            "LlamaGenerationProvider: prompt (" + std::to_string(prompt_tokens.size()) +
            " tokens) + max_tokens (" + std::to_string(max_tokens) +
            ") exceeds generation_n_ctx (" + std::to_string(n_ctx_) + ")");
    }

    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> smpl(
        llama_sampler_chain_init(llama_sampler_chain_default_params()), llama_sampler_free);
    llama_sampler_chain_add(smpl.get(), llama_sampler_init_min_p(0.05f, 1));
    llama_sampler_chain_add(smpl.get(), llama_sampler_init_temp(0.8f));
    llama_sampler_chain_add(smpl.get(), llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    std::string response;
    llama_batch batch = llama_batch_get_one(prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()));

    for (int generated = 0; generated < max_tokens; ++generated) {
        int n_ctx_used = llama_memory_seq_pos_max(llama_get_memory(ctx_), 0) + 1;
        if (n_ctx_used + batch.n_tokens > n_ctx_) {
            throw std::runtime_error("LlamaGenerationProvider: context exceeded mid-generation "
                                      "(this shouldn't happen given the preflight check above)");
        }

        if (llama_decode(ctx_, batch) != 0) {
            throw std::runtime_error("LlamaGenerationProvider: llama_decode failed");
        }

        llama_token new_token = llama_sampler_sample(smpl.get(), ctx_, -1);
        if (llama_vocab_is_eog(vocab_, new_token)) break;

        char piece_buf[256];
        int n = llama_token_to_piece(vocab_, new_token, piece_buf, sizeof(piece_buf), /*lstrip=*/0, /*special=*/true);
        if (n < 0) {
            throw std::runtime_error("LlamaGenerationProvider: failed to convert token to piece");
        }
        response.append(piece_buf, static_cast<size_t>(n));

        batch = llama_batch_get_one(&new_token, 1);
    }

    return response;
}

std::string LlamaGenerationProvider::name() const
{
    return "llama[" + model_path_ + "]";
}

} // namespace pgquarry
