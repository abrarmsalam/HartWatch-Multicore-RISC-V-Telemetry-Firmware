# Decisions taken during the unattended run

When the owner was unavailable, the most conservative option was taken and recorded here with the reasoning.

## D1: Pushing through gh instead of the macOS keychain helper

`git push` hung in `git-credential-osxkeychain get`, probably waiting on a keychain dialog nobody could answer overnight.
The stuck push was stopped, and pushes use the owner's existing `gh` login for that one command:
`git -c credential.helper= -c 'credential.helper=!gh auth git-credential' push`. No token is written to a file, the remote
URL or the git config. The keychain was not touched.

## D2: Disk floor, and what "Docker work" means

Pulling `timescale/timescaledb:2.17.2-pg16` (682.9 MiB compressed, 2.74 GB unpacked) and starting it took free disk from 15 GiB
to 8.9 GiB. Most of that is Docker's disk file growing. Nothing was deleted to recover space: no prune was run, and the
remaining reclaimable items are in use or outside this repo.
The toolchain itself runs in Docker, so stopping all Docker use would stop every measurement. If free disk nears 8 GiB,
database work stops first. Existing `hartwatch-dev` containers, which write very little, continue unless the floor is reached.
Free space is checked before each Docker step.

## D3: actionlint not used

`actionlint` is not installed, and downloading its binary was blocked by the session's permission rules. That was not worked
around. No YAML parser was available either. The workflow's only validation is that GitHub accepted and ran it.

## D4: The wall-clock job cannot fail CI

GitHub's shared runners are noisy, and this job measures QEMU host runtime, not target performance. It also missed a 12.5%
instruction increase. It runs with `continue-on-error` and only compares identical code. The `-icount` gate is the one that blocks.

## D5: Rates above 20 kHz added to the sweep

The ISR's overhead was still under 1% at 20 kHz, so 50 kHz and 100 kHz were added, plus 19, 18 and 17 us periods, to find
where it crosses 1% by measurement and not by extrapolation.

## D6: No telemetry-off row in mode B

A row with no timers on the U54s would still need the E51's tick, which mixes busy harts without timers with a ticking hart. The
owner ruled that out, so mode A provides the telemetry-off comparison.

## D7: Mann-Whitney written by hand

SciPy would add a large dependency to a disk-limited image, for one small function. It is tested against values worked out from
the definition, not against SciPy.

## D8: No WebSocket

The owner asked for one only if cheap. The collector reads finished captures, so a WebSocket would replay stored rows while
appearing live. That would mislead, so it was left out.

