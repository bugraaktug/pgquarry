-- Semantic Audience Building demo — labeling
-- For each cluster found by cluster.sql, samples a handful of its tickets'
-- per-row summaries (written by watch_generate() in schema.sql) and asks the
-- local generation model to name the group -- an ad hoc generate_sync() call,
-- since a cluster label has no single source row to write back into. Prints
-- one "found N users discussing '<label>'" line per cluster and persists the
-- label into support_ticket_clusters.

DO $$
DECLARE
    c            record;
    ticket       record;
    sample_lines text;
    prompt       text;
    v_label      text;
BEGIN
    FOR c IN
        SELECT cluster_id, count(*) AS n
        FROM support_tickets
        WHERE cluster_id IS NOT NULL
        GROUP BY cluster_id
        ORDER BY cluster_id
    LOOP
        sample_lines := '';
        FOR ticket IN
            SELECT coalesce(summary, body) AS text FROM support_tickets
            WHERE cluster_id = c.cluster_id
            ORDER BY random()
            LIMIT 8
        LOOP
            sample_lines := sample_lines || '- ' || ticket.text || E'\n';
        END LOOP;

        prompt := 'Here are one-line summaries of example customer support tickets from one group:' || E'\n' ||
                  sample_lines ||
                  E'\nIn 3 to 6 words, name the common theme these tickets share. ' ||
                  'Respond with only the name, no punctuation, no quotes.';

        -- 60s, not 20s: this prompt concatenates 8 sampled summaries
        -- (~1000+ chars), so prefill alone runs noticeably longer than the
        -- short single-ticket prompts watch_generate() enqueues per row.
        CALL pgquarry.generate_sync(prompt, 32, 60000, v_label);
        v_label := trim(both from v_label);

        UPDATE support_ticket_clusters SET label = v_label WHERE cluster_id = c.cluster_id;

        RAISE NOTICE 'found % users discussing "%"', c.n, v_label;
    END LOOP;
END $$;

SELECT cluster_id, ticket_count AS tickets, label
FROM support_ticket_clusters
ORDER BY cluster_id;
