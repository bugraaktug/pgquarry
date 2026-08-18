# Semantic Audience Building

A worked example of clustering a support-tickets-like table's embeddings into
named groups — e.g. "found 47 users discussing 'Slow API Response Times'" —
using pgquarry end to end, deliberately exercising both ways generation jobs
get created:

1. **Per-row, via `watch_generate()`** — every ticket gets auto-embedded
   *and* auto-summarized into a one-line `summary` the moment it's inserted,
   the same trigger-driven shape `watch()` already uses for embeddings.
2. **Ad hoc, via `generate_sync()`** — after clustering (a batch step with no
   single source row), each cluster is named from a sample of its tickets'
   summaries. There's no row for a cluster label to write back into, so this
   goes through the same arbitrary-text path as `embed_async()`/`embed_sync()`.

## Prerequisites

- `pgquarry` extension already created in the target database
  (`CREATE EXTENSION pgquarry CASCADE;`).
- `pgquarry_worker` running against a config with **both** `model_path` (an
  embedding GGUF) and `generation_model_path` (an instruct/chat GGUF) set.
  The repo's `pgquarry.generate-test.toml` is set up for this — from the repo
  root:

  ```bash
  ./build/pgquarry_worker --config pgquarry.generate-test.toml
  ```

## Running it

In another terminal, from the repo root:

```bash
./demo/run.sh
```

This creates `support_tickets`, seeds ~250 synthetic tickets across 5 topics,
waits for the worker to embed and summarize them all, clusters the embeddings
(k=5), and asks the local model to name each cluster from its tickets'
summaries — about 3 minutes end to end on a 4-core CPU with an optimized
build (see below). Override the target database with `PGQUARRY_DEMO_CONNINFO`,
or the ticket count with `PGQUARRY_DEMO_TICKET_COUNT` (e.g. `=50` for a
quicker smoke test — generation is single-shot per job, so summaries dominate
the wait).

Build `pgquarry_worker` with `-DCMAKE_BUILD_TYPE=RelWithDebInfo` (see main
README's Quick start) — a `Debug` build makes generation ~10-20x slower and
turns this into a much longer wait.

Or run the steps individually with `psql "$CONNINFO" -f demo/<file>.sql`, in
order: `schema.sql`, `seed.sql`, (wait for embeddings + summaries),
`cluster.sql`, `label.sql`.

## Expected output

Partway through, each ticket already has its own auto-generated summary —
worth a look before clustering runs:

```sql
SELECT id, left(body, 60) AS body, summary FROM support_tickets LIMIT 3;
```

The last step (`label.sql`) prints one line per cluster and persists each
label into `support_ticket_clusters` — a real 250-ticket run produced:

```
NOTICE:  found 69 users discussing "Token errors"
NOTICE:  found 42 users discussing "Integration with Orbit, Quarry API, Ledger API, Orbit API, Nimbus API"
NOTICE:  found 34 users discussing "Data sync discrepancies"
NOTICE:  found 53 users discussing "Duplicate balances, bill amounts"
NOTICE:  found 52 users discussing "Scheduled export"
```

The same roster stays queryable afterward:

```sql
SELECT cluster_id, ticket_count, label FROM support_ticket_clusters ORDER BY cluster_id;
```

Exact wording, counts, and even which tickets land in which cluster vary run
to run — clustering is seeded (`seed.sql`'s `setseed(0.42)`) but k-means'
random centroid init isn't, and every summary/label is the local model's own
phrasing, not scripted. Clusters won't always cleanly match the 5 synthetic
topics either (the example above merged two API-latency phrasings into one
cluster) — a small, crude k-means over real embeddings genuinely won't be
perfect, which is a more honest demo than a suspiciously clean 1:1 mapping.

## Notes

- `schema.sql`'s `summary_prompt` is a `GENERATED ALWAYS ... STORED` column,
  not a plain one: `watch_generate()`'s `prompt_column` is read verbatim as
  the generate job's `input_text` — there's no per-call instruction wrapping
  at the SQL layer, so the "summarize this" instruction has to already be
  baked into the column value.
- `cluster.sql`'s k-means is intentionally minimal (fixed `k`, fixed
  iteration count, random-row init, no convergence check) — demo-scoped, not
  part of the pgquarry extension itself.
- Re-running `run.sh` against a database that already has `support_tickets`
  will re-seed on top of existing rows; drop the table first if you want a
  clean run.
