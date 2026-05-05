# E2E Testing

The end-to-end suite uses the authored machine examples from
`../RealityEngine_AI/examples/machines/*.json`.

Run:

```bash
make e2e
```

Current verified corpus result:

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
