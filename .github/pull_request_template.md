<!-- This repository is a component of nativeps5. The contribution rules, evidence format and run rules are in the main repository:
     https://github.com/computerguy526/nativeps5 (CONTRIBUTING.md, AGENTS.md, docs/agents/evidence.md). Link them instead of repeating them.
     One change per PR. If the change also needs the main repository or another component, open those PRs too and link them below. -->

## What and why

<!-- The symptom or the need, the cause, the change. Linked PRs in nativeps5 / nativeps5-recomp / nativeps5-kyty / nativeps5-anyps5. -->

## Evidence

<!-- Delete what does not apply and say what was NOT checked. Numbers, hashes and log lines only: no game files. -->
- Unit tests: <!-- the commands you ran and their last line -->
- The entry repository's `scripts/check.sh --range <base>..HEAD --tests`, run from inside this checkout (it checks the repository it is run in): <!-- last line -->
- Title, scene and build: <!-- title ID and version, scene, run length, runs per side -->
- Before / after: <!-- frame time p50 / p95 and fps from paired runs (`nps5 report`); memory peak -->
- Picture: <!-- frame-dump comparison, if the change can affect rendering -->
- Machine and OS: <!-- e.g. M4 Pro 24 GB, macOS 26.1 -->

## Safety

- [ ] No game files, shaders, journals, frame dumps, caches or anything extracted from a game in the diff
- [ ] No secrets, personal paths or machine names
- [ ] New behaviour is off by default behind an environment switch, or the evidence shows it is neutral or better
- [ ] Upstream code is not reformatted; the change is limited to what it needs (this is a fork of an upstream project)

## AI assistance

<!-- REQUIRED, a PR without this line is not reviewed. yes / no; if yes, which agent, and confirm a person read the diff and the evidence -->
