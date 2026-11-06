#!/usr/bin/env bash
# demo.sh -- the whole pipeline in one go: boot the firmware under QEMU, capture UART1, check the
# stream, load it into TimescaleDB, and print hot functions per hart.
# Needs Docker. Run from anywhere. The database keeps running afterwards: `docker compose stop`.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== 1. Starting the database"
docker compose up -d db

echo "== 2. Building, booting 5 harts under QEMU (-icount), capturing UART1, decoding"
docker compose run --rm tools make test-fw

echo "== 3. Loading the capture and querying hot functions"
docker compose run --rm tools bash -c \
    'python3 host/collector.py build/telemetry.bin build/hartwatch.elf --name demo --timing icount --hz 1000 \
     --commit "$(git rev-parse --short HEAD)"'

echo "== Done. Start the API and dashboard with: docker compose up -d api  (http://127.0.0.1:8000)"
