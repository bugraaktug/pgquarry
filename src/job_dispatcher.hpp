#pragma once

#include <vector>

#include "job_store.hpp"

namespace pgquarry {

class EmbeddingProvider;
class GenerationProvider;
class WatchCache;

// Batches all of `jobs` through one embed_batch() call, then writes each
// result back individually. A provider-level failure (bad call, size
// mismatch) marks the whole sub-batch as error — embedding is genuinely
// batched under the hood, so there's no meaningful per-job isolation to give
// it, unlike dispatch_generate_jobs() below. Returns the number written.
int dispatch_embed_jobs(JobStore& store, EmbeddingProvider& provider, WatchCache& mappings,
                         const std::vector<JobStore::ClaimedJob>& jobs);

// Per-job, not batched (GenerationProvider::generate() is single-shot) —
// each job's failure is isolated instead of failing the whole claimed set,
// since generation is far more likely to fail on any single input than a
// batched embedding call is. gen_provider may be null (no
// generation_model_path configured), in which case every job is marked
// error rather than attempted. Returns the number written.
int dispatch_generate_jobs(JobStore& store, GenerationProvider* gen_provider, int default_max_tokens,
                            WatchCache& mappings, const std::vector<JobStore::ClaimedJob>& jobs);

} // namespace pgquarry
