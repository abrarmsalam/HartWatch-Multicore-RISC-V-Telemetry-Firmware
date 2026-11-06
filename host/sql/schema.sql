-- HartWatch schema. Run once on an empty database; every statement is safe to repeat.
--
-- ts is NOT wall-clock time. It is the firmware's mtime (microseconds since boot) placed on
-- the 1970 epoch, because a hypertable needs a time column. Under -icount that is virtual time.
-- Runs overlap in ts, so always filter by run_id.

CREATE EXTENSION IF NOT EXISTS timescaledb;

CREATE TABLE IF NOT EXISTS runs (
    run_id      SERIAL PRIMARY KEY,
    name        TEXT NOT NULL,
    git_commit  TEXT,
    timing_mode TEXT NOT NULL,          -- 'icount' or 'host'
    sample_hz   INTEGER NOT NULL,
    elf_sha256  TEXT NOT NULL,
    wire_bytes  BIGINT NOT NULL,
    bad_frames  INTEGER NOT NULL,
    ingested_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS run_harts (
    run_id     INTEGER NOT NULL REFERENCES runs ON DELETE CASCADE,
    hart       SMALLINT NOT NULL,
    samples    BIGINT NOT NULL,
    attempts   BIGINT,                  -- from the END frame; NULL if it never arrived
    dropped    BIGINT NOT NULL,
    frames     BIGINT NOT NULL,
    wire_bytes BIGINT NOT NULL,
    digest_ok  BOOLEAN,                 -- NULL if the build carries no digest
    PRIMARY KEY (run_id, hart)
);

CREATE TABLE IF NOT EXISTS samples (
    ts       TIMESTAMPTZ NOT NULL,
    run_id   INTEGER NOT NULL,
    hart     SMALLINT NOT NULL,
    seq      BIGINT NOT NULL,
    mtime    BIGINT NOT NULL,
    mcycle   BIGINT NOT NULL,
    minstret BIGINT NOT NULL,
    hpm0     BIGINT NOT NULL,           -- DTLB read miss (QEMU software TLB)
    hpm1     BIGINT NOT NULL,           -- DTLB write miss (QEMU software TLB)
    hpm2     BIGINT NOT NULL,           -- ITLB miss (QEMU software TLB)
    mepc     BIGINT NOT NULL,
    symbol   TEXT NOT NULL,
    file     TEXT,
    line     INTEGER
);

SELECT create_hypertable('samples', 'ts', chunk_time_interval => INTERVAL '1 hour', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS samples_run_hart ON samples (run_id, hart, ts);

-- One row per run, hart, function and second: how many ticks landed in that function.
CREATE MATERIALIZED VIEW IF NOT EXISTS function_time_1s
WITH (timescaledb.continuous) AS
SELECT time_bucket(INTERVAL '1 second', ts) AS bucket,
       run_id, hart, symbol,
       count(*) AS samples
FROM samples
GROUP BY bucket, run_id, hart, symbol
WITH NO DATA;
