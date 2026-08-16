-- pgquarry v1.x — installed via `CREATE EXTENSION pgquarry;` (add `CASCADE`
-- if the `vector` extension isn't already present). Supersedes the v1
-- hand-run flow in v0_schema.sql/v1_schema.sql: schema/trigger provisioning
-- now goes through pgquarry.watch() instead of a manual CREATE TRIGGER, and
-- pgquarry.embed_async()/.embed_sync() enqueue ad hoc text with no source
-- table or trigger at all.

CREATE SCHEMA IF NOT EXISTS pgquarry;

-- source_table/source_id/embed_column are NULL for ad hoc jobs enqueued via
-- embed_async() — those have no watched source row, so they write their
-- result into this table's own `result` column instead of writing back into
-- a user table.
CREATE TABLE pgquarry.jobs (
    id            bigserial PRIMARY KEY,
    status        text NOT NULL DEFAULT 'pending'
                  CHECK (status IN ('pending', 'processing', 'done', 'error')),
    source_table  text,
    source_id     bigint,
    embed_column  text,
    input_text    text NOT NULL,
    result        vector,
    error         text,
    created_at    timestamptz NOT NULL DEFAULT now(),
    updated_at    timestamptz NOT NULL DEFAULT now()
);

-- The worker upserts one row per pgquarry.toml [[table]] entry here on every
-- startup, and pgquarry.watch() upserts a row directly from SQL. Either path
-- works without the other — the worker reads this table directly to build
-- its write-back mapping, rather than only trusting its own toml config.
CREATE TABLE pgquarry.watched_tables (
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
        RAISE EXCEPTION 'pgquarry.enqueue_job: no watch registered for "%" — '
            'call pgquarry.watch(''%'', ...) or add it to pgquarry.toml and '
            '(re)start pgquarry_worker', TG_TABLE_NAME, TG_TABLE_NAME;
    END IF;

    row_id   := to_jsonb(NEW) ->> w.id_column;
    row_text := to_jsonb(NEW) ->> w.embed_column;

    INSERT INTO pgquarry.jobs (source_table, source_id, embed_column, input_text)
    VALUES (TG_TABLE_NAME, row_id::bigint, w.embed_column, row_text);

    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- SQL-side sugar for the schema/trigger provisioning sql/v1_schema.sql's
-- header comment used to walk through by hand: upserts the mapping and
-- (re)creates the trigger in one call. target_table/target_id_column default
-- to source/id_column (same-table write-back), matching config.hpp's old TOML
-- default rule. Idempotent — a deterministic trigger name means calling
-- watch() again (e.g. to change embed_column) replaces rather than stacks.
-- Parameters are p_-prefixed: plpgsql raises "ambiguous column reference" if
-- a parameter name matches a column name used bare in a query the function
-- runs, and every one of these names is also a watched_tables column.
CREATE OR REPLACE FUNCTION pgquarry.watch(
    p_source           text,
    p_embed_column     text,
    p_id_column        text DEFAULT 'id',
    p_target_table     text DEFAULT NULL,
    p_target_column    text DEFAULT NULL,
    p_target_id_column text DEFAULT NULL
) RETURNS void AS $$
DECLARE
    resolved_target_table     text := COALESCE(p_target_table, p_source);
    resolved_target_id_column text := COALESCE(p_target_id_column, p_id_column);
    trigger_name text := p_source || '_pgquarry_embed';
BEGIN
    IF p_target_column IS NULL THEN
        RAISE EXCEPTION 'pgquarry.watch: target_column is required (which column should the embedding land in?)';
    END IF;

    INSERT INTO pgquarry.watched_tables
        (source_table, id_column, embed_column, target_table, target_column, target_id_column)
    VALUES (p_source, p_id_column, p_embed_column, resolved_target_table, p_target_column, resolved_target_id_column)
    ON CONFLICT (source_table) DO UPDATE SET
        id_column        = EXCLUDED.id_column,
        embed_column      = EXCLUDED.embed_column,
        target_table      = EXCLUDED.target_table,
        target_column     = EXCLUDED.target_column,
        target_id_column  = EXCLUDED.target_id_column;

    EXECUTE format('DROP TRIGGER IF EXISTS %I ON %I', trigger_name, p_source);
    EXECUTE format(
        'CREATE TRIGGER %I AFTER INSERT OR UPDATE OF %I ON %I FOR EACH ROW EXECUTE FUNCTION pgquarry.enqueue_job()',
        trigger_name, p_embed_column, p_source
    );
END;
$$ LANGUAGE plpgsql;

-- Enqueues arbitrary text with no source table/trigger involved. The worker
-- recognizes source_table IS NULL as "ad hoc" and writes the vector into
-- jobs.result instead of doing a table write-back. p_-prefixed for the same
-- reason as watch() — input_text is also a pgquarry.jobs column.
CREATE OR REPLACE FUNCTION pgquarry.embed_async(p_input_text text) RETURNS bigint AS $$
    INSERT INTO pgquarry.jobs (input_text) VALUES (p_input_text) RETURNING id;
$$ LANGUAGE sql;

-- Blocks the calling backend until the job completes or timeout_ms elapses.
-- Postgres has no SQL/plpgsql primitive to truly block on a NOTIFY from
-- inside a routine (LISTEN only delivers to a session's libpq client between
-- statements), so this is a bounded poll against jobs.status rather than a
-- true async wait — short sleep steps, capped by wall-clock elapsed time
-- against timeout_ms.
--
-- This has to be a PROCEDURE, called via CALL rather than SELECT, not a
-- FUNCTION: the embed_async() insert and the poll loop below would otherwise
-- run inside the same transaction as the call itself, so the worker's
-- separate connection could never see the pending row (it's uncommitted)
-- until this returns — a self-deadlock that times out every single call.
-- The explicit COMMIT right after the insert is only legal inside a
-- procedure invoked at the top level (plain CALL, not nested in another
-- transaction-controlling context), and it's what makes the row visible to
-- the worker immediately.
-- p_timeout_ms has no default (and OUT stays last, its natural position):
-- Postgres rejects an OUT parameter placed after an IN parameter with a
-- DEFAULT, and — contrary to the usual "OUT params are omitted by the
-- caller" rule for plain functions — a top-level CALL to a procedure
-- requires a positional value for every declared parameter, OUT included
-- (confirmed empirically against PG16; pass NULL for it, it's discarded):
--   CALL pgquarry.embed_sync('text to embed', 5000, NULL);
CREATE OR REPLACE PROCEDURE pgquarry.embed_sync(
    IN  p_input_text  text,
    IN  p_timeout_ms  int,
    OUT result        vector
)
LANGUAGE plpgsql
AS $$
DECLARE
    job_id     bigint;
    started_at timestamptz;
    job        pgquarry.jobs%ROWTYPE;
BEGIN
    job_id := pgquarry.embed_async(p_input_text);
    COMMIT;
    started_at := clock_timestamp();

    LOOP
        SELECT * INTO job FROM pgquarry.jobs WHERE id = job_id;

        IF job.status = 'done' THEN
            result := job.result;
            RETURN;
        ELSIF job.status = 'error' THEN
            RAISE EXCEPTION 'pgquarry.embed_sync: job % failed: %', job_id, job.error;
        END IF;

        IF extract(epoch FROM clock_timestamp() - started_at) * 1000 >= p_timeout_ms THEN
            RAISE EXCEPTION 'pgquarry.embed_sync: job % did not complete within %ms — '
                'is pgquarry_worker running?', job_id, p_timeout_ms;
        END IF;

        PERFORM pg_sleep(0.05);
    END LOOP;
END;
$$;

-- CREATE EXTENSION runs as the installing role (typically a superuser), so
-- everything above is owned by that role, not the app role the worker and
-- ordinary client backends actually connect as (e.g. quser) — without these,
-- both the worker's load_watched_tables() and enqueue_job()'s own SELECT
-- (it runs as whoever's INSERT/UPDATE fired the trigger) get "permission
-- denied for schema pgquarry".
GRANT USAGE ON SCHEMA pgquarry TO PUBLIC;
GRANT SELECT, INSERT, UPDATE, DELETE ON pgquarry.jobs TO PUBLIC;
GRANT SELECT, INSERT, UPDATE ON pgquarry.watched_tables TO PUBLIC;
GRANT USAGE, SELECT ON pgquarry.jobs_id_seq TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgquarry.enqueue_job() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgquarry.watch(text, text, text, text, text, text) TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgquarry.embed_async(text) TO PUBLIC;
GRANT EXECUTE ON PROCEDURE pgquarry.embed_sync(text, int) TO PUBLIC;
