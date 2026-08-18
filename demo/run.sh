#!/usr/bin/env bash
# Semantic Audience Building demo — runs schema, seed, clustering, and
# labeling end to end against a running pgquarry_worker.
#
# Prerequisite: pgquarry_worker running against a config with both
# model_path and generation_model_path set, e.g.:
#   ./build/pgquarry_worker --config pgquarry.generate-test.toml
#
# Connection info can be overridden via PGQUARRY_DEMO_CONNINFO; defaults to
# the same DB as pgquarry.generate-test.toml. Ticket count can be overridden
# via PGQUARRY_DEMO_TICKET_COUNT (default 250) -- generation is single-shot
# per job, so a smaller count is useful for a quick smoke test.

set -euo pipefail

CONNINFO="${PGQUARRY_DEMO_CONNINFO:-host=localhost port=5432 dbname=qdb user=quser password=quser1234}"
TICKET_COUNT="${PGQUARRY_DEMO_TICKET_COUNT:-250}"
DEMO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> Creating schema and registering watch_generate()"
psql "$CONNINFO" -v ON_ERROR_STOP=1 -f "$DEMO_DIR/schema.sql"

echo "==> Seeding $TICKET_COUNT synthetic tickets"
psql "$CONNINFO" -v ON_ERROR_STOP=1 -v ticket_count="$TICKET_COUNT" -f "$DEMO_DIR/seed.sql"

echo "==> Waiting for per-row embeddings + summaries (pgquarry_worker must be running)"
# Generation is single-shot per job (no batching, unlike embed_batch()), so
# 250 rows' worth of summaries takes noticeably longer than their embeddings.
MAX_CHECKS=600
for i in $(seq 1 $MAX_CHECKS); do
    remaining=$(psql "$CONNINFO" -t -A -c "SELECT count(*) FROM support_tickets WHERE embedding IS NULL OR summary IS NULL")
    if [ "$remaining" -eq 0 ]; then
        echo "    all embeddings + summaries done"
        break
    fi
    if [ $((i % 10)) -eq 0 ] || [ "$i" -eq 1 ]; then
        echo "    $remaining tickets still pending..."
    fi
    if [ "$i" -eq "$MAX_CHECKS" ]; then
        echo "Timed out waiting for embeddings/summaries -- is pgquarry_worker running against a config with model_path and generation_model_path set?" >&2
        exit 1
    fi
    sleep 2
done

echo "==> Clustering (k-means-ish over embeddings)"
psql "$CONNINFO" -v ON_ERROR_STOP=1 -f "$DEMO_DIR/cluster.sql"

echo "==> Labeling clusters (first real generate_sync() usage)"
psql "$CONNINFO" -v ON_ERROR_STOP=1 -f "$DEMO_DIR/label.sql"
