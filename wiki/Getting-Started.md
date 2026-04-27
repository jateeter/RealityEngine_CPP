# Getting Started

## Prerequisites

- macOS or Linux with a C++20 compiler.
- `make`.
- `curl`.
- `python3` for startup verification and E2E summary parsing.
- `localAIStack` checked out beside this repo if you want shared Qdrant
  verification to pass without `--allow-missing-qdrant`.
- `RealityEngine_AI` checked out beside this repo for the machine corpus.

Expected sibling layout:

```text
GitHub/
  RealityEngine_AI/
  RealityEngine_CPP/
  localAIStack/
```

## Build

```bash
make
```

## Test

```bash
make test
make e2e
```

## Run

```bash
./start.sh
```

The default service URLs are:

```text
Reality Engine    http://localhost:3000
Perception Engine http://localhost:3001
Qdrant            http://localhost:4333
```

Stop services:

```bash
./stop.sh
```

