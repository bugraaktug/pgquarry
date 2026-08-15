-- pgquarry v0 — hand-run once against the target database.
-- No extension, no trigger: enqueue jobs yourself with INSERT, the worker
-- polls this table directly.

CREATE EXTENSION IF NOT EXISTS vector;
CREATE SCHEMA IF NOT EXISTS pgquarry;

CREATE TABLE pgquarry.jobs (
    id          bigserial PRIMARY KEY,
    status      text NOT NULL DEFAULT 'pending'
                CHECK (status IN ('pending', 'processing', 'done', 'error')),
    input_text  text NOT NULL,
    embedding   vector(1024),   -- bge-m3 dims; adjust to match your model
    error       text,
    created_at  timestamptz NOT NULL DEFAULT now(),
    updated_at  timestamptz NOT NULL DEFAULT now()
);
