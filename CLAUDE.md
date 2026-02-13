# T-Deck Device Automation Guide (Claude)

Use this flow for all on-device firmware verification.

## One-Time Setup
```bash
uv sync
```

## Preferred Smoke Test
```bash
pio run -e T-Deck-Pro-debug -t upload
uv run scripts/agent_smoke.py --boot-wait 2
```

`agent_smoke.py` performs write + render + capture + basic checks and prints `PASS`/`FAIL`.
Use `--marker` without `0` if you need a custom marker.
Default camera source is `http://10.0.44.199:4747/`.

## If Camera Selection Is Wrong
```bash
uv run scripts/probe_cameras.py --max-index 5
```

Use a working index (`opened=1`, `frame=1`, non-zero mean/std) as `--camera-source "<idx>"`.

## Manual Scenario Path
1. Flash debug firmware:
   `pio run -e T-Deck-Pro-debug -t upload`
2. Serial channel check:
   `uv run scripts/tdeck_agent.py --boot-wait 2 "PING" "STATE"`
3. Drive scenario:
   `uv run scripts/tdeck_agent.py "CMD new" "TEXT test-123" "RENDER" "WAIT 1200" "STATE"`
4. Capture artifact:
   `uv run scripts/capture_webcam.py --image artifacts/test-123.jpg`
5. Evaluate from protocol results + artifact.

Keypress-driven examples:
- Open command palette: `uv run scripts/tdeck_agent.py "MIC SINGLE" "WAIT 500" "STATE"`
- Toggle terminal (SSH attempt path): `uv run scripts/tdeck_agent.py "MIC DOUBLE" "WAIT 300" "STATE"`
- Generic repeated press: `uv run scripts/tdeck_agent.py "PRESS MIC 2" "WAIT 300" "STATE"`

## Notes
- Production build: `pio run -e T-Deck-Pro -t upload` (automation protocol off).
- Keep automation-only code under `#if TDECK_AGENT_DEBUG`.
