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
implementation.

Current passing corpus:

```text
machines loaded:       54
input sequences run:   149
input steps run:       674
outputs observed:      336
metadata assertions:   65
```

The runner asserts explicit metadata when present:

- `expectedOutputs`
- `expectedOutputCount`
- `expectedOutputVector`
- `expectedOutputSequence`

