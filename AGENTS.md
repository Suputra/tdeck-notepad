# T-Deck Agent Workflow (Codex/OpenAI)

## Goal
Run a closed hardware loop without manual keypresses:
1. Flash debug firmware.
2. Drive input over serial.
3. Capture screen evidence via camera feed (default: `http://10.0.44.199:4747/`).
4. Decide pass/fail and iterate.

## Build Modes
- Production: `pio run -e T-Deck-Pro -t upload`
- Debug automation: `pio run -e T-Deck-Pro-debug -t upload`

`T-Deck-Pro-debug` enables `TDECK_AGENT_DEBUG=1` (serial automation protocol).
Production keeps it disabled to avoid runtime overhead.

## Required Tools
- PlatformIO (`pio`)
- `uv` for Python deps and script execution

One-time setup:
```bash
uv sync
```

## Fast Path (Preferred)
Use this when you want a quick write+capture verification:

```bash
pio run -e T-Deck-Pro-debug -t upload
uv run scripts/agent_smoke.py --boot-wait 2
```

What `agent_smoke.py` does:
1. Clears notepad.
2. Types a marker string.
3. Forces render and waits.
4. Captures a camera image from the default IP camera feed.
5. Verifies serial `text_len` matches marker length.
6. Verifies capture is not black/invalid.
7. Prints `PASS` or `FAIL`.

Notes:
- It does not OCR text yet. You still confirm marker visibility in the image.
- Output image path is printed at the end.
- If custom marker text is provided, avoid `0` (current `TEXT` emulation limitation).

## Camera Setup
If using a local webcam and captures are black/blank or the wrong device is used:

```bash
uv run scripts/probe_cameras.py --max-index 5
```

Pick an index with:
- `opened=1`
- `frame=1`
- non-trivial `mean/std` (not near zero)

Then pass that index via `--camera-source "<idx>"` (or deprecated `--camera-device`).

## Full Canonical Loop
1. Flash debug firmware:
   `pio run -e T-Deck-Pro-debug -t upload`
2. Smoke-check agent channel:
   `uv run scripts/tdeck_agent.py --boot-wait 2 "PING" "STATE"`
3. Drive scenario over serial commands.
4. Capture artifacts:
   - image (default IP camera): `uv run scripts/capture_webcam.py --image artifacts/<name>.jpg`
   - image (custom source): `uv run scripts/capture_webcam.py --source "<url-or-idx>" --image artifacts/<name>.jpg`
   - video (custom source): `uv run scripts/capture_webcam.py --source "<url-or-idx>" --video artifacts/<name>.mp4 --duration 8`
5. Evaluate:
   - no `AGENT ERR`
   - expected state transitions (`AGENT OK STATE ...`)
   - expected screen content in image/video
6. Patch and repeat.

## Serial Protocol (Debug Firmware Only)
Send one command per line with `@` prefix.

- `@PING`
- `@HELP`
- `@STATE`
- `@KEY <row> <col_rev>`
- `@PRESS <token> [count]`
- `@TEXT <text-with-escapes>`
- `@CMD <device-command-mode-command>`
- `@WAIT <ms>`
- `@RENDER`
- `@BOOTOFF`

Escapes in `TEXT`: `\\n`, `\\r`, `\\t`, `\\\\`, `\\s`.

`PRESS` tokens:
- Special: `MIC`, `ALT`, `SYM`, `LSHIFT`, `RSHIFT`, `SPACE`, `ENTER`, `BACKSPACE`
- Single-char physical key tokens: letters and symbol-key positions, e.g. `q`, `w`, `1`, `?`, `-`

Quick keypress examples:
```bash
uv run scripts/tdeck_agent.py "PRESS MIC" "WAIT 500" "STATE"      # opens command mode
uv run scripts/tdeck_agent.py "CMD ssh" "WAIT 300" "STATE"        # switches to terminal/SSH mode
uv run scripts/tdeck_agent.py "CMD np" "WAIT 300" "STATE"         # returns to notepad mode
```

## Troubleshooting
- `AGENT ERR`: treat as scenario failure; fix command or firmware behavior.
- Camera opened but frame is black: verify IP stream URL, or for local webcam use `scripts/probe_cameras.py` and switch `--camera-source`; increase `--warmup`.
- `TEXT` fails due to active modifier mode: use explicit `KEY`/`PRESS` commands first.
- `TEXT` fails on marker character `0`: use marker text without `0` or type `0` via key-level commands.
- Need release validation: flash production env and rerun manual or non-agent checks.

## Guardrails
- Use debug env for automation unless release/prod validation is requested.
- Keep automation-only firmware code inside `#if TDECK_AGENT_DEBUG`.
