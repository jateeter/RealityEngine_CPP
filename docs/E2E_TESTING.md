# E2E Testing

The end-to-end runner uses the authored machine examples from
`../RealityEngine_AI/examples/machines/*.json`.

Run:

```bash
make e2e
```

Current verified corpus result:

```text
machines loaded:       54
input sequences run:   149
input steps run:       674
outputs observed:      336
metadata assertions:   65
```

The test performs these checks:

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
