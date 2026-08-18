-- Semantic Audience Building demo — synthetic seed data
-- Tickets across 5 recognizable topics (3 phrase templates each), with
-- customer/product/number substitution for variety. setseed() makes the run
-- reproducible. Row count comes from the ticket_count psql variable
-- (run.sh passes it, default 250 -- see PGQUARRY_DEMO_TICKET_COUNT).

SELECT setseed(0.42);

-- Each array pick is a plain scalar expression in the SELECT list, not a
-- CROSS JOIN LATERAL subquery: a LATERAL subquery that doesn't actually
-- reference the outer row (none of these do) isn't true correlation, so the
-- planner is free to hoist it out of the nested loop and evaluate it once
-- for the whole query -- confirmed via EXPLAIN (a one-row Result feeding the
-- loop) -- which silently gave every row the same template/customer/product.
INSERT INTO support_tickets (body)
SELECT
    replace(
        replace(
            replace(
                (ARRAY[
                    -- slow API response
                    'Hi, {customer} here. Every call to the {product} API has been taking over {n}ms today, way slower than usual. Can someone look into this?',
                    '{product} API latency has spiked for us -- requests that used to return instantly are now hanging for {n}ms plus. Is there an incident?',
                    'Our integration with {product} is timing out. Response times went from fast to {n}ms overnight. Please investigate.',
                    -- billing / invoice discrepancies
                    'This month invoice for {product} shows a charge of ${n} I do not recognize. Can you break down what this is for, {customer}?',
                    'I was double billed on my {product} account -- ${n} charged twice this cycle. Please refund the duplicate.',
                    'The invoice total for {product} does not match what I was quoted. Off by ${n}. Need this corrected.',
                    -- login / auth failures
                    'I cannot log into my {product} account anymore. Getting an invalid credentials error even after resetting my password, {n} times now.',
                    '{customer} is locked out of {product} -- 2FA codes are not arriving by SMS. Tried {n} times.',
                    'SSO login to {product} keeps failing with a token error. Started happening about {n} minutes ago.',
                    -- export / feature requests
                    'Would love a CSV export option for {product} reports -- currently have to copy {n} rows manually one at a time.',
                    'Feature request: can {product} support scheduled exports? Doing this manually every week for {n} accounts is painful.',
                    'It would help a lot if {product} let us export data in bulk instead of {n} records at a time.',
                    -- data sync bugs
                    'Data sync between {product} and our warehouse dropped {n} rows overnight. Numbers do not reconcile.',
                    '{product} sync job seems to be stuck -- last successful sync was {n} hours ago.',
                    'We are seeing duplicate records after the {product} sync ran twice on the same {n} rows.'
                ])[1 + (random() * 14)::int],
                '{customer}', (ARRAY['Alex', 'Jordan', 'Sam', 'Taylor', 'Morgan', 'Casey', 'Riley', 'Jamie', 'Drew', 'Avery'])[1 + (random() * 9)::int]
            ),
            '{product}', (ARRAY['Quarry', 'Orbit', 'Nimbus', 'Ledger', 'Beacon'])[1 + (random() * 4)::int]
        ),
        '{n}', (10 + (random() * 990)::int)::text
    )
FROM generate_series(1, :ticket_count) AS g(i);
