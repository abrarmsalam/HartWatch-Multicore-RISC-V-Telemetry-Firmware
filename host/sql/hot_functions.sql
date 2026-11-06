-- Hot functions per hart for one run: the share of timer ticks that landed in each function.
-- This is statistical sampling: it estimates where a hart spent its time, it does not measure it.
SELECT f.hart,
       f.symbol,
       sum(f.samples) AS samples,
       round(100.0 * sum(f.samples) / sum(sum(f.samples)) OVER (PARTITION BY f.hart), 2) AS percent
FROM function_time_1s f
WHERE f.run_id = %(run_id)s
GROUP BY f.hart, f.symbol
ORDER BY f.hart, samples DESC;
