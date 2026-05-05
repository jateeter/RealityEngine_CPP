# E2E Testing

Run:

```bash
make e2e
```

The E2E runner loads all JSON machines from:

```text
../RealityEngine_AI/examples/machines
```

It then runs every authored `inputSequences` entry through the C++ machine
implementation and validates the service process boundary.

Current passing corpus:

```text
machines loaded:       906
input sequences run:   4211
input steps run:       6963
outputs observed:      3596
metadata assertions:   4115

active domains:        10
domain cases run:      100
domain input steps:    222
domain outputs:        134
```

The runner asserts explicit metadata when present:

- `expectedOutputs`
- `expectedOutputCount`
- `expectedOutputVector`
- `expectedOutputSequence`

`make e2e` also starts the Reality Engine and Perception Engine binaries on
test ports, checks both health endpoints, confirms machine loading through
`/api/machines`, and posts through Perception Engine `/api/push` to verify the
PE-to-RE HTTP path. Override `REALITY_ENGINE_E2E_PORT` and
`PERCEPTION_ENGINE_E2E_PORT` when the default `3299`/`3301` ports are in use.

The service test imports a deterministic three-machine chain:

```text
PE test source [4600:4602] -> Chain A output [4602:4604]
Chain A output [4602:4604] -> Chain B output [4604:4606]
Chain B output [4604:4606] -> Chain C output [4606:4608]
```

Successive `/api/push` calls assert that upstream outputs remain in the
Perception Engine stream and trigger downstream machines on later pushes.
