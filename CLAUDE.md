# T-Deck Device Automation Guide (Claude)

Use this flow for all on-device firmware verification.

## One-Time Setup
```bash
uv sync
```

## Targets
- T-Deck Pro (default): `pio run` / `pio run -e debug`.
- reTerminal E1001: `pio run -e rt` / `pio run -e rtdebug` (aliases of `reterminal` / `reterminal-debug`). Same firmware,
  selected by the `BOARD_RETERMINAL` capability flags in `src/firmware/device.h`. No physical
  keyboard — input arrives over BLE from `scripts/companion.py`. Full guide: `docs/07-reterminal-companion.md`.
  The serial automation protocol below works on `reterminal-debug` too (use `@TEXT`/`@RENDER`/`@CMD`/`@STATE`,
  `@I2CSCAN` should show 0x51/0x44/0x6B). `@PRESS`/`@KEY` matrix tokens are T-Deck-only.

## Preferred Smoke Test
```bash
pio run -e debug                      # reTerminal: pio run -e rtdebug  (aliases upload by default)
uv run scripts/agent_smoke.py --boot-wait 2
```

`agent_smoke.py` performs write + render + capture + basic checks and prints `PASS`/`FAIL`.
Use `--marker` without `0` if you need a custom marker.
Default camera source is `http://localhost:8100/stream`.

## If Camera Selection Is Wrong
```bash
uv run scripts/probe_cameras.py --max-index 5
```

Use a working index (`opened=1`, `frame=1`, non-zero mean/std) as `--camera-source "<idx>"`.

## Manual Scenario Path
1. Flash debug firmware:
   `pio run -e debug -t upload`
2. Serial channel check:
   `uv run scripts/tdeck_agent.py --boot-wait 2 "PING" "STATE"`
3. Drive scenario:
   `uv run scripts/tdeck_agent.py "CMD rm __manual__.txt" "CMD edit __manual__.txt" "TEXT test-123" "RENDER" "WAIT 1200" "STATE"`
4. Capture artifact:
   `uv run scripts/capture_webcam.py --image artifacts/test-123.jpg`
5. Evaluate from protocol results + artifact.

Keypress-driven examples:
- Open command palette: `uv run scripts/tdeck_agent.py "PRESS MIC" "WAIT 500" "STATE"`
- Open terminal (SSH attempt path): `uv run scripts/tdeck_agent.py "CMD ssh" "WAIT 300" "STATE"`
- Return to notepad: `uv run scripts/tdeck_agent.py "CMD np" "WAIT 300" "STATE"`
- Generic repeated press: `uv run scripts/tdeck_agent.py "PRESS MIC 2" "WAIT 300" "STATE"`

## Camera Mux
Default camera source is `localhost:8100` (persistent MJPEG relay for DroidCam).
- Setup: `bash ~/camera/install.sh`
- Health check: `curl localhost:8100/health`
- Logs: `journalctl --user -u camera-mux -f`

## Notes
- Production build: `pio run -t upload` (automation protocol off).
- Keep automation-only code under `#if TDECK_AGENT_DEBUG`.
- Keep `README.md` synchronized with current behavior, command set, config format, and test flow.
- Keep README edits concise and practical; avoid LLM-style phrasing and section bloat.
