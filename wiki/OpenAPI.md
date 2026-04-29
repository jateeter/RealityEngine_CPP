# OpenAPI

The repository includes OpenAPI 3.0 contracts for both native services:

- `docs/openapi/reality-engine.yaml`
- `docs/openapi/perception-engine.yaml`

Default service URLs:

```text
Reality Engine    http://localhost:3299
Perception Engine http://localhost:3300
```

The specs cover the currently mounted C++ route surface, including runtime
options, async Perception Engine push jobs, localAIStack integration endpoints,
machine CRUD, perceptual simulation, and `/api/perceive`.

Use either file directly with Swagger UI, Redoc, OpenAPI Generator, or other
OpenAPI-compatible tooling.
