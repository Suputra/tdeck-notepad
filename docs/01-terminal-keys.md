# Terminal Control Keys

## Status: DONE

## Problem
The terminal mode only sends basic printable characters, Enter, and Backspace.
Missing: Escape, Ctrl+key combos, Tab — making interactive terminal use impractical.
Can't exit programs (Ctrl+C), send EOF (Ctrl+D), suspend (Ctrl+Z), or clear screen (Ctrl+L).

## Design
Use Alt+key combos to send control characters over SSH:
- **Alt + letter** → Ctrl+letter (e.g., Alt+C → Ctrl+C = 0x03)
- **Alt + Space** → Escape (0x1B)
- **Alt + Enter** → Tab (0x09)

This reuses the existing alt_mode toggle. When Alt is active:
- Touch taps still send arrow keys
- Letter keys send Ctrl+letter (ASCII 1-26)
- Space sends Escape
- Enter sends Tab
- Existing Alt+S/Q/W shortcuts preserved

## Key Mapping (Alt+key in terminal mode)
| Key | Sends | Use |
|-----|-------|-----|
| Alt+C | 0x03 | Ctrl+C (interrupt) |
| Alt+D | 0x04 | Ctrl+D (EOF) |
| Alt+Z | 0x1A | Ctrl+Z (suspend) |
| Alt+L | 0x0C | Ctrl+L (clear screen) |
| Alt+A | 0x01 | Ctrl+A (tmux/screen prefix) |
| Alt+Space | 0x1B | Escape |
| Alt+Enter | 0x09 | Tab |

Directional arrows are independent touch-tap gestures.
