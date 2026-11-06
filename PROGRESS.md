# Progress (unattended run, Days 4-7)

Re-read this file and CLAUDE.md after any context compaction. Order: Day 4, Day 6, Day 5 (minimal), Day 7.
Branch: days-4-7. Push after each completed day. make test before each commit. No force, no prune, no v1.0 tag.
Stop only for a destructive or irreversible action. Decisions go in docs/DECISIONS.md.
Toolchain: `docker run --rm -v "$PWD":/work hartwatch-dev <cmd>`. Disk must stay above 8 GiB free for Docker work.

## Done
- Day 3 commits pushed to main (via gh credentials, see D1 in docs/DECISIONS.md).
- Day 4: symbols, collector, schema, hot functions from a real run. DB tests: 6 passed.
- Day 7: DESIGN.md, README.md, scripts/demo.sh.
- Day 5: API, dashboard, compose. Run 42172406712 on GitHub Actions succeeded.
- Day 6: bench image, gate demo, wall-clock job, sweep. Results in docs/benchmarks.md.

## In progress
- Nothing. Days 4-7 complete, report written. Waiting for owner review.

## Next
- Owner reviews docs/MORNING_REPORT.md and decides on merging days-4-7.

## Notes for resuming
- Push: git -c credential.helper= -c 'credential.helper=!gh auth git-credential' push origin days-4-7
- DB: docker compose up -d db; docker compose run --rm tools <cmd> (HW_DB_URL is set there). Run 2 = day4-1khz.
- Check free disk before Docker steps. Floor 8 GiB. It was 8.9 GiB after Day 4.

## Open problems
- Disk margin is small. See D2.
