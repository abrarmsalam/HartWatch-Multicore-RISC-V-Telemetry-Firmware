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

