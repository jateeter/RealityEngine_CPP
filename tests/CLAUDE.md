# RealityEngine_CPP Tests Guidance

This directory contains C++ unit, integration, and e2e tests.

- Use `make test` for local logic and `make e2e` for runtime/corpus/API behavior.
- Keep OpenClaw, MQTT, corpus loading, and byte-equivalence failures reported separately.
- Prefer explicit endpoint/env setup when tests talk to live services.

## Standing rules — authoritative in `../../RealityEngine_CI/docs/ENGINEERING_CONTRACT.md`

These apply here and are **not** restated in this file. They were previously
copied into eighteen `CLAUDE.md` files across six repositories, which is the
duplication problem the rules themselves warn about: copies drift, a rule added
to one applies only where someone looked, and with no authority a reader cannot
tell which copy is current.

| Rule | In short |
| --- | --- |
| Qualify every "registry" | Never the bare word — instance / machine / cesgen / arbitration / domain / semantic-bus / tag. |
| Verify a merge beyond the hosted checks | A green PR is not a verified PR; the hosted path cannot reach the integration points. Name what you could not exercise, and record what you noticed but did not chase. |
| Never commit to main | Branch from `origin/main`, PR, verify, squash-merge, clean up. |
| _CI is the authority | Peripheral repos keep minimal CI that forces local validation; RealityEngine_CI verifies fixes against a live universe. Check its `docs/` before adding CI anywhere else. |
| Use bash, not zsh | Shell work runs in `/opt/homebrew/bin/bash` (5.x), not zsh or macOS `/bin/bash` 3.2: any loop, unquoted variable, glob or `set --` goes through it with `set -euo pipefail`, and you check the command's exit status, not the pipeline tail. |

Read the contract for the full text, the qualifier table, and the cleanup steps.
