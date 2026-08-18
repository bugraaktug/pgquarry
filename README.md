# pgquarry

**Postgres-native embeddings, one `CREATE EXTENSION` away.**

pgquarry turns any Postgres table into a self-embedding table: watch a column, and every `INSERT`/`UPDATE` gets its vector filled in automatically — no application code, no CDC pipeline, no external ML service to run alongside Postgres. The same queue can also generate text (summaries, labels, any short completion) via a second local model, dispatched from the same trigger or driven ad hoc from SQL.

If you need your embeddings kept in sync with an existing table's live change stream via logical replication (WAL) — no trigger, no touching the source schema — reach for [walkrie](https://github.com/bugraaktug/walkrie) instead. pgquarry is for when a trigger is fine and you want a plain job-queue mechanism you can also drive directly from SQL or application code (`embed_async()`/`embed_sync()`), not only from row changes.

## How it works

The load-bearing decision is keeping inference out of the Postgres backend process — a synchronous SQL function calling into `llama.cpp` directly would tie up a backend connection for the full length of inference. Instead:

```
client INSERT / trigger  →  pgquarry.jobs table
                                    ↓
                    worker: SELECT ... FOR UPDATE SKIP LOCKED
                                    ↓
                          dispatch on job_type
                              ↓            ↓
              llama.cpp embed_batch()   llama.cpp generate() (chat-templated, single-shot)
                              ↓            ↓
                    UPDATE/UPSERT result(_text) + pg_notify
```

The worker (`pgquarry_worker`) is a separate OS process, not an in-postmaster background worker — a crash inside `llama.cpp` should never take down the whole Postgres instance the way a bgworker crash would (postmaster treats any bgworker crash like a backend crash: full cluster restart). pgvector stays a separate, maintained dependency — pgquarry only populates the `vector` column pgvector provides; search still goes through pgvector's own operators and indexes.

## Feature set

- **`CREATE EXTENSION pgquarry;`** provisions the schema, job table, and SQL API in one step — nothing to hand-run.
- **`pgquarry.watch(source, embed_column, ...)`** registers a trigger and a write-back mapping from SQL alone — no config file edit, no worker restart. Write-back is a same-table `UPDATE` by default, or an `UPSERT` into a separate `target_table` for a chunk/embedding-table split.
- **`pgquarry.watch_generate(source, embed_column, embed_target_column, prompt_column, generate_target_column, ...)`** registers the same kind of trigger, but for *both* an embed and a generate job per row change in one shot — e.g. auto-fill an `embedding` column and a `summary` column from the same `INSERT`.
- **`pgquarry.embed_async(text)` / `CALL pgquarry.embed_sync(text, timeout_ms)`** and their generation counterparts **`pgquarry.generate_async(text, max_tokens)` / `CALL pgquarry.generate_sync(text, max_tokens, timeout_ms)`** enqueue arbitrary text with no source table or trigger involved — fire-and-forget or block-and-wait, straight from SQL or application code.
- **Config-driven worker** (`pgquarry.toml`): connection info, local GGUF model path/threads/context/batch size, poll interval, log file/level/rotation, and an optional second `generation_model_path` (instruct/chat GGUF) for text generation — `generate`/`watch_generate()` jobs fail with an actionable error rather than blocking startup if it's unset.
- **Two-stage retention**: `status='done'` job rows stick around as an audit trail until `[retention] purge_after`, then the worker purges them on its own sweep (`purge_verbose` for per-row vs. summary logging).
- **`pg_notify('pgquarry_jobs', ...)`** fires after every batch write, so other listeners don't have to poll for completion either.
- **Local inference**: a dependency-light C++ binary running GGUF models in-process via `llama.cpp` — nothing to deploy but one worker and one job table, no data leaving your infrastructure.

## Quick start

1. Build:

   ```bash
   git clone --recurse-submodules https://github.com/bugraaktug/pgquarry.git
   cd pgquarry
   cmake -S . -B build
   cmake --build build -j"$(nproc)"
   sudo cmake --build build --target install   # places pgquarry--1.0.sql/.control in Postgres's extension dir
   ```

   Needs `libpq`, `spdlog`, and a Postgres with `pgvector` installed. `toml++` and `llama.cpp` are vendored as submodules.

   For an optimized build, pass `-DCMAKE_BUILD_TYPE=RelWithDebInfo` to the `cmake -S . -B build` step — a `Debug` build is noticeably slower, especially for generation.

2. Enable the extension (needs a superuser session — `CREATE EXTENSION` isn't marked `trusted` yet):

   ```sql
   CREATE EXTENSION pgquarry CASCADE;   -- CASCADE pulls in `vector` if it isn't installed yet
   ```

   The extension script grants `PUBLIC` everything it needs (schema usage, table access, `EXECUTE` on its functions) — your app role doesn't need any manual `GRANT`s for pgquarry's own objects. It does need to own whichever tables you register with `watch()`, same as any other table it writes to.

3. Copy `pgquarry.toml.example` to `pgquarry.toml`, point it at your database and a local GGUF model (add `generation_model_path` too if you want generation — any instruct/chat GGUF with a baked-in chat template), then start the worker:

   ```bash
   ./build/pgquarry_worker --config pgquarry.toml
   ```

4. Register a watch and insert a row:

   ```sql
   CREATE TABLE docs (id bigserial PRIMARY KEY, body text NOT NULL, embedding vector(1024));
   SELECT pgquarry.watch('docs', 'body', 'id', NULL, 'embedding');
   INSERT INTO docs (body) VALUES ('this row will have a vector in a moment');
   -- SELECT embedding FROM docs WHERE id = 1;  -- filled in by the worker
   ```

   Or register a cross-table watch — the worker `UPSERT`s into a separate table instead of augmenting the source row (e.g. a chunk/embedding-table split):

   ```sql
   CREATE TABLE docs_chunks (id bigserial PRIMARY KEY, chunk_text text NOT NULL);
   CREATE TABLE chunk_embeddings (chunk_id bigint PRIMARY KEY, embedding vector(1024));
   SELECT pgquarry.watch('docs_chunks', 'chunk_text', 'id', 'chunk_embeddings', 'embedding', 'chunk_id');
   INSERT INTO docs_chunks (chunk_text) VALUES ('this chunk gets a row in chunk_embeddings');
   ```

   Or skip the table and trigger entirely for ad hoc text:

   ```sql
   -- Fire-and-forget: enqueue and move on, check back later via jobs.id
   SELECT pgquarry.embed_async('embed this later');

   -- Or block until it's done (or times out)
   CALL pgquarry.embed_sync('embed this on demand', 5000);
   ```

5. Generation works the same way, from a watched column or ad hoc. `watch_generate()` fills in an embedding *and* a generated column from the same row change. `prompt_column` is read verbatim as the generate job's input — no instruction wrapping happens at the SQL layer, so bake the instruction in yourself (a `GENERATED ALWAYS ... STORED` column works well):

   ```sql
   CREATE TABLE tickets (
       id        bigserial PRIMARY KEY,
       body      text NOT NULL,
       prompt    text GENERATED ALWAYS AS ('Summarize this ticket in one sentence: ' || body) STORED,
       embedding vector(1024),
       summary   text
   );
   SELECT pgquarry.watch_generate('tickets', 'body', 'embedding', 'prompt', 'summary');
   INSERT INTO tickets (body) VALUES ('customer reports slow API responses since this morning');
   -- SELECT embedding, summary FROM tickets WHERE id = 1;  -- both filled in by the worker
   ```

   Or ad hoc, mirroring `embed_async()`/`embed_sync()`:

   ```sql
   SELECT pgquarry.generate_async('Summarize this ticket in 5 words: ...');
   CALL pgquarry.generate_sync('Summarize this ticket in 5 words: ...', NULL, 15000);
   ```

   See [`demo/`](demo/) for a full worked example (clustering embeddings, then naming each cluster via generation).

## Known limitations

- **Deleting a watched source row doesn't cancel or clean up its jobs.** The trigger only fires on `INSERT`/`UPDATE`; a `DELETE` leaves any in-flight `pgquarry.jobs` row for that id in place. For same-table write-back the eventual `UPDATE` just affects zero rows. For cross-table write-back (`target_table` != source) the eventual `UPSERT` will *insert* a new row into `target_table` keyed to an id whose source row no longer exists — an orphan embedding row you're responsible for cleaning up if that matters to your schema.

## Roadmap

- **v1.x** (current): `CREATE EXTENSION` provisioning, `pgquarry.watch()` / `embed_async()` / `embed_sync()` SQL API, plus text generation via `watch_generate()` / `generate_async()` / `generate_sync()` against a second local instruct/chat GGUF.
- **v2**: OpenAI-compatible API support as an alternative backend to local GGUF/`llama.cpp` inference, for both embedding and generation.

## License

Apache-2.0. Portions of the embedding-provider code are adapted from [walkrie](https://github.com/bugraaktug/walkrie) (also Apache-2.0). Developed with AI assistance via [Claude Code](https://claude.com/claude-code).
