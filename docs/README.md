# Project documentation

The source for the Demon's Souls documentation lives in this repository. Add new findings here, and update the current reports as experiments complete.

## Current guidance

- [Performance handoff ? September 10, 2026](performance-handoff-2026-09-10.md)
- [Workaround, setup, and limitations](demons-souls-workaround.md)
- [Performance measurements and optimization validation](demons-souls-performance.md)
- [Local runtime folder and launchers](local-runtime.md)

## Investigation archive

These dated notes preserve the evidence and intermediate conclusions from earlier debugging. They include local artifact paths and historical process state; use the current guidance above for the latest status.

- [CPU fault investigation](investigations/cpu-fault-investigation.md)
- [Collision-list race report](investigations/demons-souls-touch-race-report.md)
- [GPU crash report](investigations/demons-souls-crash-report.md)
- [NVIDIA fault-detail capture](investigations/nv-fault-details-outcome.md)
- [Save-data investigation](investigations/savedata-investigation.md)
- [GitHub and Reddit research](investigations/github-reddit-research-2026-09-10.md)
- [Earlier debugging notebook](investigations/early-debugging-notes.md)

## Windows release notes

- [Merged upstream build, fffb4ce](releases/des-experimental-2026-09-10-fffb4ce.md)
- [First workaround release, 7be5d03](releases/des-experimental-2026-09-10-7be5d03.md)

[Documentation migration records](documentation-migration.json) preserve the original paths and pre-edit hashes of the ten documents consolidated from build/runtime folders. Generated release-package copies remain part of their archived packages. Raw captures, binaries, logs, and saves retain their existing local artifact paths.
