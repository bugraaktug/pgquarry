#pragma once

#include <memory>
#include <string>

namespace pgquarry {

struct GenerationConfig; // forward decl — avoid circular include with generation_config.hpp

class GenerationProvider
{
public:
    virtual ~GenerationProvider() = default;

    virtual void init() = 0;
    // Single-shot: prompt in, generated text out. No batch method — unlike
    // embedding's embed_batch(), parallel-sequence decode batching (distinct
    // EOS/length per sequence) is materially harder and isn't needed for v1.
    virtual std::string generate(const std::string& prompt, int max_tokens) = 0;
    virtual std::string name() const = 0;
};

// v1: only "llama" is a valid provider — throws for anything else. OpenAI
// support is v2 (see README's roadmap) — job_type/SQL API/schema don't
// change, only which provider make_generation_provider() returns.
std::shared_ptr<GenerationProvider> make_generation_provider(const GenerationConfig& cfg);

} // namespace pgquarry
