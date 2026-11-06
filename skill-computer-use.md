---
name: computer-use
description: How to reliably operate desktop apps and web pages (screenshots, clicking, typing, UI tree, browser refs). Read before any multi-step GUI task.
---

# Operating the computer reliably

## Pick the right tool
1. **Website?** Use `browser_navigate` → `browser_snapshot` → `browser_click` / `browser_type` with refs (`e12`). Fastest and most reliable. Use `browser_screenshot` only when layout/images/captchas matter.
2. **Windows desktop app?** Try `ui_tree` (lists buttons/boxes by index) and act with `ui_act`. It needs no vision and is exact.
3. **Anything else / UI Automation shows nothing?** Use `computer`: `screenshot` → act → read the new screenshot.
4. **No GUI needed?** Prefer `run_command`, `read_file`, `web_extract`.

## Loop
- Observe (screenshot / snapshot) → one action → observe again. Never chain blind clicks.
- After each action check the result actually happened; if not, try a different approach instead of repeating.
- Use `computer zoom` for small text. Coordinates are always in pixels of the latest screenshot.
- Open apps with `launch_app`, then `computer focus_window` with part of the title.
- Typing uses the clipboard, so Persian/Unicode text is fine. Use `key` for shortcuts ("ctrl+s", "enter", "alt+f4").

## Safety
- Don't send messages, make purchases, delete data or submit forms unless the user asked for exactly that.
- If a login, captcha, 2FA or payment screen appears, stop and ask the user to handle it.
- Report what you did and what you saw, briefly.
