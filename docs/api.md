# API and dashboard

## What it does

`host/api.py` is a small FastAPI service over the database, and `host/static/index.html` is one plain page that uses it.
`docker compose up` starts the database and the API on `http://127.0.0.1:8000`. OpenAPI docs are at `/docs` and `/openapi.json`.

| Endpoint | Returns |
| --- | --- |
| `GET /runs` | Every run, newest first |
| `GET /runs/{id}` | One run with per-hart totals |
| `GET /runs/{id}/harts/{hart}/series?metric=&limit=` | Change in a counter between consecutive samples |
| `GET /runs/{id}/hot-functions` | Share of ticks per function, per hart |
| `GET /runs/{id}/diff/{baseline}` | Change in those shares from a baseline run |

## Why this design

- **`Store` is an interface.** Tests swap in a fake, so API behaviour is tested with no database in `make test`, and
  `host/test_db.py` tests the real SQL separately.
- **`metric` goes through a whitelist.** A column name can't be a SQL parameter, so it has to go into the SQL text. Only values
  from the `METRICS` table ever reach it, and a test checks that an injection attempt stops at the API.
- **The diff is pure.** `diff_hot` takes two lists and returns rows, so it needs no database to test.
- **No WebSocket.** The collector reads finished captures, so "live" would only replay stored rows (decision D8).

## What could go wrong

- **It has no authentication**, by design. It listens on localhost only. Don't expose it.
- **The diff compares sample shares.** That is not the CI gate, and a small change can be sampling noise.
- **`/docs` loads Swagger UI from a CDN**, so it needs internet access. `/openapi.json` does not.
- **The dashboard has not been viewed in a browser.** It was served and its API calls were tested, but its rendering was not
  checked.
