# pgquarry

A Postgres-native async job queue for local LLM inference — GGUF embedding via `llama.cpp`, dequeued by an external worker, no CDC and no external ML service.

Split out of [walkrie](https://github.com/bugraaktug/walkrie)'s embedding-provider layer: same proven, batched, local-model embedding code (dims checking, batch validation, crash-safe claim/resume), triggered by a job table instead of a WAL stream.

## Why this, not walkrie

Walkrie's job is CDC: tail the WAL, embed rows that change, write them to a sink, confirm the LSN. pgquarry drops the WAL entirely — inference is triggered by a row landing in a job table (an explicit `INSERT` or a trigger on any table), not by logical replication. What carries over is the part of walkrie that has nothing to do with replication: the embedding provider abstraction and the discipline around it.

Where `pgai`, `pg_vectorize`, and PostgresML lean on an external API or a Python/ML service next to Postgres, pgquarry's inference engine is a small, dependency-light C++ binary running GGUF models in-process to itself — nothing to deploy but one worker and one job table.

## Architecture

The load-bearing decision is keeping inference out of the Postgres backend process — a synchronous SQL function calling into `llama.cpp` directly would tie up a backend connection for the full length of inference. Instead:

```
client INSERT / trigger  →  pgquarry.jobs table
                                    ↓
                    worker: SELECT ... FOR UPDATE SKIP LOCKED
                                    ↓
                         llama.cpp batched embed_batch()
                                    ↓
                    UPDATE/UPSERT result + pg_notify
```

The worker is a separate OS process (not an in-postmaster background worker) — a crash inside `llama.cpp` should never take down the whole Postgres instance the way a bgworker crash would (postmaster treats any bgworker crash like a backend crash: full cluster restart). This is the same reason walkrie's backfill worker is a separate process rather than a thread, and it's the same shape `pgai`'s vectorizer worker and `pg_vectorize`'s `vectorize-worker` already ship in production.

pgvector stays a separate, maintained dependency — pgquarry only populates the `vector` column pgvector provides; search still goes through pgvector's own operators and indexes.

## Roadmap

- **v0** — prove the loop. No extension, no trigger, no `pg_notify`. A job goes in by hand (`INSERT INTO pgquarry.jobs`), the worker fills it in, you poll for the result. Goal: confirm the transplanted embedding path works standalone, outside walkrie's WAL-driven process model.
- **v1** — usable in a real workflow. Config-driven worker (`pgquarry.toml`), a generic trigger enqueues on insert/update, results land in a `vector` column via `UPDATE` (same table) or `UPSERT` (a separate `target_table`, keyed by `target_id_column`), completion is `NOTIFY` instead of polling. Job retention: `status='done'` rows are kept as an audit trail until older than `[retention] purge_after`, then purged by the worker's own poll loop (`purge_verbose` controls per-row vs. summary logging).
- **v1.x** — `CREATE EXTENSION pgquarry` as sugar over the same mechanism (schema/trigger provisioning, `pgquarry.watch()`/`.embed_async()`/`.embed_sync()`). The worker stays external; `embed_sync()` only adds a bounded `LISTEN` wait in the calling backend.
- **v2** — OpenAI as a second provider, reusing walkrie's `OpenAIProvider` behind the same `embed()`/`embed_batch()` interface `LlamaProvider` uses. One provider per worker process to start; per-job provider selection is a later, demand-driven decision.

## Non-goals (v1)

- Cloud providers before v2 — local GGUF only until the core loop is proven.
- CDC or replication triggers of any kind — that's walkrie's job, deliberately not duplicated here.
- Completion/chat models, RAG orchestration, hybrid-search helper functions.
- A true in-postmaster background worker.
- Cascading deletes into a separate `target_table` when a source row is removed — pgquarry only triggers on INSERT/UPDATE, so orphan cleanup after a source `DELETE` is the user's own FK/`ON DELETE CASCADE`.

## Code reuse from walkrie (Apache-2.0)

| File | How |
|---|---|
| `embedding_provider.hpp/.cpp` | Reused near-verbatim — the provider interface is exactly what a job-queue worker needs. |
| `llama_provider.hpp/.cpp` | Reused near-verbatim — local GGUF batched embedding, the core of v1. |
| `openai_provider.hpp/.cpp` | Reused near-verbatim, for v2. |
| `caching_embedding_provider.hpp/.cpp` | Reused if useful — job-level dedup. |
| `backfill_worker.cpp`, `backfill_store.cpp` | Reused as a pattern (claim/lease loop, stale-claim reset, crash-resume), not a copy — schema differs. |
| `config.hpp` — `AppConfig::validate()`, `TableMapping`/`ColumnMapping` | Reused as convention/pattern — specific validation errors, and source/target table+id separation. |
| `CMakeLists.txt`, `third_party/llama.cpp` submodule setup | Reused wholesale. |

Not carried over: `pgoutput_parser`, `wal_frame`, `replication_source`, `event_dispatcher`, `qdrant_sink` — replication/sink-specific, out of scope for a job-queue product.

## Status

Pre-v0. Planning only — no code yet.
