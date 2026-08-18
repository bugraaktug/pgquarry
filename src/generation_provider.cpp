#include "generation_provider.hpp"
#include "generation_config.hpp"
#include "llama_generation_provider.hpp"

#include <stdexcept>

namespace pgquarry {

std::shared_ptr<GenerationProvider> make_generation_provider(const GenerationConfig& cfg)
{
    if (cfg.provider == "llama") {
        return std::make_shared<LlamaGenerationProvider>(cfg);
    }
    throw std::runtime_error(
        "make_generation_provider: unknown provider '" + cfg.provider + "' — v1 only implements 'llama'");
}

} // namespace pgquarry
