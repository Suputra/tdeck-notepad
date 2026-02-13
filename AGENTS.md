# T-Deck Agent Workflow (Codex/OpenAI)

## Goal
Run a closed hardware loop without manual keypresses:
1. Flash debug firmware.
2. Drive input over serial.
3. Capture screen evidence via webcam.
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
uv run scripts/agent_smoke.py --camera-device 1 --boot-wait 2
```

What `agent_smoke.py` does:
1. Clears notepad.
2. Types a marker string.
3. Forces render and waits.
4. Captures a webcam image.
5. Verifies serial `text_len` matches marker length.
6. Verifies capture is not black/invalid.
7. Prints `PASS` or `FAIL`.

Notes:
- It does not OCR text yet. You still confirm marker visibility in the image.
- Output image path is printed at the end.
- If custom marker text is provided, avoid `0` (current `TEXT` emulation limitation).

## Camera Setup
If captures are black/blank or wrong camera is used:

```bash
uv run scripts/probe_cameras.py --max-index 5
```

Pick an index with:
- `opened=1`
- `frame=1`
- non-trivial `mean/std` (not near zero)

Then pass that index via `--camera-device`.

## Full Canonical Loop
1. Flash debug firmware:
   `pio run -e T-Deck-Pro-debug -t upload`
2. Smoke-check agent channel:
   `uv run scripts/tdeck_agent.py --boot-wait 2 "PING" "STATE"`
3. Drive scenario over serial commands.
4. Capture artifacts:
   - image: `uv run scripts/capture_webcam.py --device <idx> --image artifacts/<name>.jpg`
   - video: `uv run scripts/capture_webcam.py --device <idx> --video artifacts/<name>.mp4 --duration 8`
5. Evaluate:
   - no `AGENT ERR`
   - expected state transitions (`AGENT OK STATE ...`)
   - expected screen content in image/video
6. Patch and repeat.

## Serial Protocol (Debug Firmware Only)
Send one command per line with `@` prefix.

- `@PING`
- `@HELP`
- `@MODE`
- `@STATE`
- `@KEY <row> <col_rev>`
- `@KEYNAME <special|single-char>`
- `@PRESS <token> [count]`
- `@CHAR <char>`
- `@TEXT <text-with-escapes>`
- `@MIC SINGLE`
- `@MIC DOUBLE`
- `@CMD <device-command-mode-command>`
- `@WAIT <ms>`
- `@RENDER`
- `@BOOTOFF`

Escapes in `TEXT`/`CHAR`: `\\n`, `\\r`, `\\t`, `\\\\`, `\\s`.

`KEYNAME`/`PRESS` tokens:
- Special: `MIC`, `ALT`, `SYM`, `LSHIFT`, `RSHIFT`, `SPACE`, `ENTER`, `BACKSPACE`
- Single-char physical key tokens: letters and symbol-key positions, e.g. `q`, `w`, `1`, `?`, `-`

Quick keypress examples:
```bash
uv run scripts/tdeck_agent.py "MIC SINGLE" "WAIT 500" "STATE"     # opens command mode
uv run scripts/tdeck_agent.py "MIC DOUBLE" "WAIT 300" "STATE"     # toggles terminal/notepad
uv run scripts/tdeck_agent.py "PRESS MIC 2" "WAIT 300" "STATE"    # double-tap via generic press
```

## Troubleshooting
- `AGENT ERR`: treat as scenario failure; fix command or firmware behavior.
- Camera opened but frame is black: use `scripts/probe_cameras.py`, switch `--camera-device`, increase `--warmup`.
- `TEXT` fails due to active modifier mode: use explicit `KEY`/`KEYNAME` commands first.
- `TEXT` fails on marker character `0`: use marker text without `0` or type `0` via key-level commands.
- Need release validation: flash production env and rerun manual or non-agent checks.

## Guardrails
- Use debug env for automation unless release/prod validation is requested.
- Keep automation-only firmware code inside `#if TDECK_AGENT_DEBUG`.
