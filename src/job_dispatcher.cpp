#include "job_dispatcher.hpp"

#include "embedding_provider.hpp"
#include "generation_provider.hpp"
#include "watch_cache.hpp"

#include <functional>
#include <spdlog/spdlog.h>

namespace pgquarry {

namespace {

// Runs write_fn (the JobStore write call for this job's result), then marks
// it done and bumps written on success, or marks it error with the
// exception's message on failure. mark_done_after is false for ad hoc writes
// (kWriteAdHocResult(Text) already sets status='done' in the same statement)
// and true for mapped writes (write_back(_text) doesn't touch status itself)
// — the one difference between dispatch_embed_jobs' and dispatch_generate_jobs'
// otherwise-identical write/finish shape.
void finish_write(JobStore& store, long long job_id, int& written, bool mark_done_after,
                   const std::string& debug_msg, const std::function<void()>& write_fn)
{
    try {
        write_fn();
        if (mark_done_after) store.mark_done(job_id);
        ++written;
        spdlog::debug("[worker] job {} {}", job_id, debug_msg);
    } catch (const std::exception& e) {
        spdlog::error("[worker] write failed for job {}: {}", job_id, e.what());
        store.mark_error(job_id, e.what());
    }
}

} // namespace

int dispatch_embed_jobs(JobStore& store, EmbeddingProvider& provider, WatchCache& mappings,
                         const std::vector<JobStore::ClaimedJob>& jobs)
{
    if (jobs.empty()) return 0;

    std::vector<std::string> texts;
    texts.reserve(jobs.size());
    for (const auto& job : jobs) texts.push_back(job.input_text);

    std::vector<std::vector<float>> vectors;
    try {
        vectors = provider.embed_batch(texts);
    } catch (const std::exception& e) {
        spdlog::error("[worker] embed_batch failed for a batch of {}: {}", jobs.size(), e.what());
        for (const auto& job : jobs) store.mark_error(job.id, e.what());
        return 0;
    }

    if (!vectors.empty() && vectors.size() != jobs.size()) {
        spdlog::error("[worker] embed_batch returned {} vectors for {} jobs — marking batch as error",
                      vectors.size(), jobs.size());
        for (const auto& job : jobs) store.mark_error(job.id, "embed_batch size mismatch");
        return 0;
    }

    int written = 0;
    for (size_t i = 0; i < vectors.size(); ++i) {
        const auto& job = jobs[i];
        if (vectors[i].empty()) {
            store.mark_error(job.id, "no embedding produced (see worker log for tokenization details)");
            continue;
        }

        if (!job.source_table.has_value()) {
            // Ad hoc job (pgquarry.embed_async()) — no source table, result goes into the job row itself.
            finish_write(store, job.id, written, /*mark_done_after=*/false, "(ad hoc) written to jobs.result",
                         [&] { store.write_ad_hoc_result(job.id, vectors[i]); });
            continue;
        }

        auto it = mappings.find(*job.source_table, "embed");
        if (it == mappings.end()) {
            store.mark_error(job.id, "no embed watch registered for '" + *job.source_table +
                                      "' — was it removed since this job was enqueued?");
            continue;
        }

        finish_write(store, job.id, written, /*mark_done_after=*/true,
                     "written to " + it->second.target_table + "." + it->second.target_column,
                     [&] { store.write_back(it->second, *job.source_id, vectors[i]); });
    }
    return written;
}

int dispatch_generate_jobs(JobStore& store, GenerationProvider* gen_provider, int default_max_tokens,
                            WatchCache& mappings, const std::vector<JobStore::ClaimedJob>& jobs)
{
    int written = 0;
    for (const auto& job : jobs) {
        if (!gen_provider) {
            store.mark_error(job.id, "[worker] generation_model_path is not set in pgquarry.toml");
            continue;
        }

        int max_tokens = job.max_tokens.value_or(default_max_tokens);
        std::string result_text;
        try {
            result_text = gen_provider->generate(job.input_text, max_tokens);
        } catch (const std::exception& e) {
            spdlog::error("[worker] generate failed for job {}: {}", job.id, e.what());
            store.mark_error(job.id, e.what());
            continue;
        }
        if (result_text.empty()) {
            store.mark_error(job.id, "no text generated (see worker log for details)");
            continue;
        }

        if (!job.source_table.has_value()) {
            // Ad hoc job (pgquarry.generate_async()) — no source table, result goes into the job row itself.
            finish_write(store, job.id, written, /*mark_done_after=*/false, "(ad hoc) written to jobs.result_text",
                         [&] { store.write_ad_hoc_result_text(job.id, result_text); });
            continue;
        }

        auto it = mappings.find(*job.source_table, "generate");
        if (it == mappings.end()) {
            store.mark_error(job.id, "no generate watch registered for '" + *job.source_table +
                                      "' — was it removed since this job was enqueued?");
            continue;
        }

        finish_write(store, job.id, written, /*mark_done_after=*/true,
                     "written to " + it->second.target_table + "." + it->second.target_column,
                     [&] { store.write_back_text(it->second, *job.source_id, result_text); });
    }
    return written;
}

} // namespace pgquarry
