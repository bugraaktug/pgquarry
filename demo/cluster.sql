-- Semantic Audience Building demo — clustering
-- Bare-bones k-means over support_tickets.embedding using pgvector's <->
-- operator and avg(vector) (pgvector >= 0.5.0). Fixed k, fixed iteration
-- count, random-row init, no convergence check -- k-means-ish, not a
-- general-purpose implementation, and demo-scoped only.

DO $$
DECLARE
    k          constant int := 5;
    iterations constant int := 10;
    i          int;
BEGIN
    CREATE TEMP TABLE centroids (
        cluster_id int PRIMARY KEY,
        embedding  vector(1024)
    ) ON COMMIT DROP;

    INSERT INTO centroids (cluster_id, embedding)
    SELECT row_number() OVER () - 1, embedding
    FROM support_tickets
    WHERE embedding IS NOT NULL
    ORDER BY random()
    LIMIT k;

    FOR i IN 1..iterations LOOP
        -- Assign each ticket to its nearest centroid.
        UPDATE support_tickets t
        SET cluster_id = nearest.cluster_id
        FROM (
            SELECT DISTINCT ON (st.id) st.id, c.cluster_id
            FROM support_tickets st
            CROSS JOIN centroids c
            WHERE st.embedding IS NOT NULL
            ORDER BY st.id, c.embedding <-> st.embedding
        ) nearest
        WHERE t.id = nearest.id;

        -- Recompute each centroid as the mean of its assigned tickets.
        UPDATE centroids c
        SET embedding = m.avg_embedding
        FROM (
            SELECT cluster_id, avg(embedding) AS avg_embedding
            FROM support_tickets
            WHERE cluster_id IS NOT NULL
            GROUP BY cluster_id
        ) m
        WHERE c.cluster_id = m.cluster_id;
    END LOOP;
END $$;

-- Re-derive the persisted cluster roster from scratch -- cluster_id
-- assignments above are fresh every run, so any previous roster (and its
-- labels) is stale.
TRUNCATE support_ticket_clusters;

INSERT INTO support_ticket_clusters (cluster_id, ticket_count)
SELECT cluster_id, count(*)
FROM support_tickets
WHERE cluster_id IS NOT NULL
GROUP BY cluster_id;

SELECT cluster_id, ticket_count AS tickets
FROM support_ticket_clusters
ORDER BY cluster_id;
