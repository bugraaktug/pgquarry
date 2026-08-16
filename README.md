# pgquarry

**Postgres-native embeddings, one `CREATE EXTENSION` away.**

pgquarry turns any Postgres table into a self-embedding table: watch a column, and every `INSERT`/`UPDATE` gets its vector filled in automatically — no application code, no CDC pipeline, no external ML service to run alongside Postgres.

## Why pgquarry, not walkrie

Reach for [walkrie](https://github.com/bugraaktug/walkrie) when you need embeddings kept in sync with an existing table's live change stream via logical replication (WAL) — no trigger, no touching the source schema. Reach for pgquarry when a trigger is fine and you want a plain job-queue mechanism you can also drive directly from SQL or application code (`embed_async()`/`embed_sync()`), not only from row changes.

## How it works

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

The worker (`pgquarry_worker`) is a separate OS process, not an in-postmaster background worker — a crash inside `llama.cpp` should never take down the whole Postgres instance the way a bgworker crash would (postmaster treats any bgworker crash like a backend crash: full cluster restart). pgvector stays a separate, maintained dependency — pgquarry only populates the `vector` column pgvector provides; search still goes through pgvector's own operators and indexes.

## Feature set

- **`CREATE EXTENSION pgquarry;`** provisions the schema, job table, and SQL API in one step — nothing to hand-run.
- **`pgquarry.watch(source, embed_column, ...)`** registers a trigger and a write-back mapping from SQL alone — no config file edit, no worker restart. Write-back is a same-table `UPDATE` by default, or an `UPSERT` into a separate `target_table` for a chunk/embedding-table split.
- **`pgquarry.embed_async(text)` / `CALL pgquarry.embed_sync(text, timeout_ms)`** enqueue arbitrary text with no source table or trigger involved — fire-and-forget or block-and-wait, straight from SQL or application code.
- **Config-driven worker** (`pgquarry.toml`): connection info, local GGUF model path/threads/context/batch size, poll interval, and log file/level/rotation.
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

2. Enable the extension (needs a superuser session — `CREATE EXTENSION` isn't marked `trusted` yet):

   ```sql
   CREATE EXTENSION pgquarry CASCADE;   -- CASCADE pulls in `vector` if it isn't installed yet
   ```

   The extension script grants `PUBLIC` everything it needs (schema usage, table access, `EXECUTE` on its functions) — your app role doesn't need any manual `GRANT`s for pgquarry's own objects. It does need to own whichever tables you register with `watch()`, same as any other table it writes to.

3. Copy `pgquarry.toml.example` to `pgquarry.toml`, point it at your database and a local GGUF model, then start the worker:

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

   Or skip the table and trigger entirely for ad hoc text:

   ```sql
   CALL pgquarry.embed_sync('embed this on demand', 5000);
   ```

## License

Apache-2.0. Portions of the embedding-provider code are adapted from [walkrie](https://github.com/bugraaktug/walkrie) (also Apache-2.0).
