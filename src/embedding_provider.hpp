// Adapted from walkrie (Apache-2.0): src/embedding_provider.hpp
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace pgquarry {

struct EmbeddingConfig; // forward decl — avoid circular include with embedding_config.hpp

class EmbeddingProvider
{
public:
    virtual ~EmbeddingProvider() = default;

    virtual void init() = 0;
    virtual std::vector<float> embed(const std::string& text) = 0;
    virtual std::vector<std::vector<float>> embed_batch(const std::vector<std::string>& texts);
    virtual int dimensions() const = 0;
    virtual std::string name() const = 0;
};

// v0: only "llama" is a valid provider — throws for anything else. OpenAI
// support is v2 (see README's roadmap).
std::shared_ptr<EmbeddingProvider> make_embedding_provider(const EmbeddingConfig& cfg);

} // namespace pgquarry
