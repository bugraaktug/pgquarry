-- pgquarry v1 — hand-run once against the target database. Supersedes
-- v0_schema.sql: pgquarry.jobs no longer holds the result itself (that now
-- lands in the user's own table via the worker's write-back), so it drops
-- and recreates the table with the v1 shape rather than migrating it.
--
-- After this: attach pgquarry.enqueue_job() to whichever tables you want
-- watched (see pgquarry.toml.example for the matching [[table]] config),
-- e.g.:
--   CREATE TRIGGER docs_embed
--       AFTER INSERT OR UPDATE OF body ON docs
--       FOR EACH ROW EXECUTE FUNCTION pgquarry.enqueue_job();

CREATE EXTENSION IF NOT EXISTS vector;
CREATE SCHEMA IF NOT EXISTS pgquarry;

DROP TABLE IF EXISTS pgquarry.jobs;
CREATE TABLE pgquarry.jobs (
    id            bigserial PRIMARY KEY,
    status        text NOT NULL DEFAULT 'pending'
                  CHECK (status IN ('pending', 'processing', 'done', 'error')),
    source_table  text NOT NULL,
    source_id     bigint NOT NULL,
    embed_column  text NOT NULL,
    input_text    text NOT NULL,
    error         text,
    created_at    timestamptz NOT NULL DEFAULT now(),
    updated_at    timestamptz NOT NULL DEFAULT now()
);

-- The worker upserts one row per pgquarry.toml [[table]] entry here on
-- every startup. enqueue_job() looks itself up by TG_TABLE_NAME so the
-- trigger below can stay argument-free.
CREATE TABLE IF NOT EXISTS pgquarry.watched_tables (
    source_table      text PRIMARY KEY,
    id_column         text NOT NULL,
    embed_column      text NOT NULL,
    target_table      text NOT NULL,
    target_column     text NOT NULL,
    target_id_column  text NOT NULL
);

CREATE OR REPLACE FUNCTION pgquarry.enqueue_job() RETURNS trigger AS $$
DECLARE
    w pgquarry.watched_tables%ROWTYPE;
    row_id   text;
    row_text text;
BEGIN
    SELECT * INTO w FROM pgquarry.watched_tables WHERE source_table = TG_TABLE_NAME;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'pgquarry.enqueue_job: no [[table]] mapping registered for "%" — '
            'add it to pgquarry.toml and (re)start pgquarry_worker so it can sync '
            'pgquarry.watched_tables', TG_TABLE_NAME;
    END IF;

    row_id   := to_jsonb(NEW) ->> w.id_column;
    row_text := to_jsonb(NEW) ->> w.embed_column;

    INSERT INTO pgquarry.jobs (source_table, source_id, embed_column, input_text)
    VALUES (TG_TABLE_NAME, row_id::bigint, w.embed_column, row_text);

    RETURN NEW;
END;
$$ LANGUAGE plpgsql;
