-- Semantic Audience Building demo — schema
-- Run: psql "$CONNINFO" -f demo/schema.sql

CREATE TABLE IF NOT EXISTS support_tickets (
    id             bigserial PRIMARY KEY,
    body           text NOT NULL,
    -- watch_generate()'s prompt_column is read verbatim as the generate
    -- job's input_text -- there's no per-call instruction wrapping at the
    -- SQL layer, so the instruction has to already be baked into the column
    -- value. A generated column does that once, at insert time.
    summary_prompt text GENERATED ALWAYS AS (
        'Summarize this support ticket in one short sentence: ' || body
    ) STORED,
    embedding      vector(1024),
    summary        text,
    cluster_id     int
);

-- cluster.sql (re)populates this each run, keyed by the cluster_id it just
-- assigned; label.sql fills in `label` afterward. Durable home for the
-- generated cluster names -- label.sql's RAISE NOTICE output doesn't
-- persist anywhere on its own.
CREATE TABLE IF NOT EXISTS support_ticket_clusters (
    cluster_id   int PRIMARY KEY,
    ticket_count int NOT NULL,
    label        text
);

-- Auto-embed body -> embedding AND auto-summarize summary_prompt -> summary
-- on every insert via pgquarry_worker, in one trigger.
SELECT pgquarry.watch_generate(
    'support_tickets',
    'body',            -- embed_column
    'embedding',        -- embed_target_column
    'summary_prompt',   -- prompt_column (generate input)
    'summary'           -- generate_target_column
);
