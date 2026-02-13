# Project Skills

This file defines reusable hardware-debug skills for AI agents working on this repo.

## `camera-probe`
Use when camera index selection is unknown or captures are black.

Command:
```bash
uv run scripts/probe_cameras.py --max-index 5
```

Pass criteria:
- At least one index reports `opened=1` and `frame=1`.
- Selected index has non-trivial `mean/std` values.

## `flash-debug`
Use when firmware changes must be tested on-device with automation enabled.

Command:
```bash
pio run -e T-Deck-Pro-debug -t upload
```

Output expectation:
- Flash succeeds.
- Device boots and responds to `@PING`.

## `serial-smoke`
Use after every debug flash to verify command channel health.

Command:
```bash
uv sync
uv run scripts/tdeck_agent.py --boot-wait 2 "PING" "STATE"
```

Pass criteria:
- `AGENT OK PONG`
- `AGENT OK STATE ...`

## `write-capture-smoke`
Use as the default end-to-end verification of write+render+capture.

Command:
```bash
uv run scripts/agent_smoke.py --camera-device <idx> --boot-wait 2
```

Pass criteria:
- Script exits successfully and prints `[smoke] PASS`.
- Artifact image path is produced.
- Marker is visibly readable in the captured image.

Constraint:
- Custom `--marker` should avoid character `0` for now.

## `scenario-drive`
Use to emulate device input and mode transitions without touching hardware keys.

Available primitives:
- `MIC SINGLE` / `MIC DOUBLE`
- `KEY row col_rev`
- `KEYNAME ...` (supports special names and single-char physical key tokens)
- `PRESS token [count]`
- `CHAR ...`
- `TEXT ...`
- `CMD ...`
- `WAIT ...`
- `STATE`

Example:
```bash
uv run scripts/tdeck_agent.py "CMD new" "TEXT ls\\n" "RENDER" "WAIT 400" "STATE"
```

Mode-control examples:
```bash
uv run scripts/tdeck_agent.py "MIC SINGLE" "WAIT 500" "STATE"
uv run scripts/tdeck_agent.py "MIC DOUBLE" "WAIT 300" "STATE"
uv run scripts/tdeck_agent.py "PRESS MIC 2" "WAIT 300" "STATE"
```

## `visual-capture`
Use to verify rendered e-ink behavior with external camera evidence.

Commands:
```bash
uv run scripts/capture_webcam.py --image artifacts/frame.jpg
uv run scripts/capture_webcam.py --video artifacts/run.mp4 --duration 10
```

Pass criteria:
- Artifact file is written.
- Frame/video clearly contains device screen with expected content.

## `release-flash`
Use only for production validation or release candidates.

Command:
```bash
pio run -e T-Deck-Pro -t upload
```

Constraint:
- Do not use production build for agent-driven serial automation checks; automation protocol is disabled there.
