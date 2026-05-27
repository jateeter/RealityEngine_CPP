# E2E Testing

The end-to-end suite uses the authored machine examples from
`../RealityEngine_Machines/machines/*.json`.

Run:

```bash
make e2e
```

Current verified corpus result:

```text
machines loaded:       1006
input sequences run:   4716
input steps run:       7573
outputs observed:      4001
metadata assertions:   4620

active domains:        11
domain cases run:      110
domain input steps:    234
domain outputs:        142
```

The corpus tests perform these checks:

- Loads every machine JSON file with the C++ `MachineLoader` equivalent.
- Finds each machine's `inputSequences`.
- Resets the machine for each input sequence.
- Feeds every sequence vector into `Machine::process_input`.
- Counts emitted machine outputs.
- Applies metadata assertions when present:
  - `expectedOutputs`
  - `expectedOutputCount`
  - `expectedOutputVector`
  - `expectedOutputSequence`

Human-readable `expectedOutputs` objects are treated as scenario notes unless
they also include one of the explicit numeric or vector fields above.

`expectedOutputs` numeric values are treated as a minimum output count because
some authored AI infrastructure scenarios include a baseline healthy output
before the scenario-specific output. `expectedOutputCount` remains an exact
count assertion.

`make e2e` also runs `tests/e2e_services.sh`, which starts the Reality Engine
and Perception Engine binaries on test ports, validates both health endpoints,
confirms the Reality Engine loaded machines through `/api/machines`, and posts
through Perception Engine `/api/push` to verify the PE-to-RE HTTP process
boundary. Override `REALITY_ENGINE_E2E_PORT` and
`PERCEPTION_ENGINE_E2E_PORT` when the default `3299`/`3301` ports are in use.

The service test also imports a deterministic three-machine chain on unused
high-dimensional regions:

```text
PE test source [4600:4602] -> Chain A output [4602:4604]
Chain A output [4602:4604] -> Chain B output [4604:4606]
Chain B output [4604:4606] -> Chain C output [4606:4608]
```

Successive `/api/push` calls assert that the Perception Engine preserves each
Reality Engine output in the assembled perceptual stream, allowing downstream
machines to consume it on later pushes.

## HealthKit Spezi Bridge

`make e2e-healthkit-spezi` runs the PE-side contract for a native iOS bridge
built with Stanford SpeziHealthKit. It starts RE and PE on isolated test ports,
loads `config/integrations.healthkit-spezi.example.json`, requires a bridge
token, and posts normalized read-only samples for:

- HealthKit blood pressure correlation
- HealthKit workout/exercise
- HealthKit sleep analysis

The test verifies token rejection, registry loading, source mapping resolution,
sensor source updates, and downstream RE machine output regions:

```text
Blood pressure source [4320:4324] -> output [4350:4352]
Exercise source       [4330:4334] -> output [4352:4354]
Sleep source          [4340:4344] -> output [4354:4356]
```

The native iOS app remains outside this C++ repo. It owns HealthKit
entitlements, user authorization, background collection, anchored reads, and
unit normalization; PE receives only authorized normalized bridge payloads.
