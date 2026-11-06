-- The busiest source lines for one run, from DWARF. Reads samples directly, because the
-- continuous aggregate groups by function only.
SELECT hart, symbol, file, line, count(*) AS samples
FROM samples
WHERE run_id = %(run_id)s
GROUP BY hart, symbol, file, line
ORDER BY hart, samples DESC;
