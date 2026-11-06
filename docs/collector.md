# Collector and database

## What it does

`host/collector.py` takes a finished UART1 capture and its ELF, decodes it with `host/decoder.py`, symbolizes each sample
and loads it into TimescaleDB. `host/sql/schema.sql` defines the tables. `host/sql/hot_functions.sql` is the headline query.

| Table | Holds |
| --- | --- |
| `runs` | One row per capture: name, commit, timing mode, sample rate, ELF hash |
| `run_harts` | Per-hart totals: samples, attempts, drops, wire bytes, digest result |
| `samples` | Hypertable, one row per sample |
| `function_time_1s` | Continuous aggregate: ticks per run, hart, function and second |

## Why this design

- **One transaction per run.** A failure leaves nothing half-loaded.
- **Decoding is reused.** One decoder means one definition of the format.
- **Pure functions do the work.** Building rows needs no database, so it is unit-tested without one. `host/test_db.py` then
  tests the real thing, and is skipped unless `HW_DB_URL` is set.
- **Values too big for `BIGINT` are refused, not wrapped.** Silent corruption would be worse than a failed load.

## What `ts` means

A hypertable needs a time column, but firmware time is not wall-clock time. `ts` is `mtime` (microseconds since boot) placed
on the 1970 epoch. Under `-icount` that is virtual time. Every run starts near 1970, so runs overlap and queries must filter
by `run_id`.

## What could go wrong

- **It reads finished captures only.** There is no live follow mode.
- **The aggregate needs a refresh.** The collector does it after each load. Without that, new data would not appear.
- **"Time per function" is an estimate** from sample counts: ticks in a function, times the sample period.
- **Credentials in `docker-compose.yml`** are for local use only. The project has no authentication by design.
