# SceneChat — Xbox Client

Native original Xbox homebrew chat client for SceneChat. Built with the RXDK SDK targeting 733MHz Xbox hardware.

**Team Resurgent / Darkone83**

---

## Requirements

- Original Microsoft Xbox (2001) with modchip or softmod
- RXDK (Repackaged Xbox Development Kit)
- MSVC 2003 / Xbox SDK toolchain
- USB keyboard (optional — on-screen keyboard available for controller-only setups)

---

## Building

Open the RXDK project in Visual Studio 2003. All source files are flat in the project root — no subdirectories.

```
Build → Build Solution
```

Output is a standard Xbox XBE. Deploy to your Xbox via FTP or copy to a disc/USB.

---

## Controls

| Input | Action |
|-------|--------|
| D-pad up/down | Scroll messages (chat focus) / select room (sidebar focus) |
| D-pad left/right | Switch focus between sidebar and chat |
| A / Left Trigger | Join selected room |
| Y | Open on-screen keyboard |
| Start | Send message |
| B | Clear input |
| R3 (right stick click) | Toggle online users overlay |
| Back | Return to login screen |

**USB keyboard:**
- Type directly into the input bar at any time
- Enter sends the message
- Escape clears the input

---

## Features

- Full DH + ChaCha20-Poly1305 encrypted connection
- User registration and login
- Auto-login via saved credentials on the Xbox hard drive (`D:\creds.dat`)
- Multi-room chat with real-time messages
- Message word wrap and newline rendering
- Real-time deleted message sync — admin deletions update on-screen instantly
- Inline emoji rendering via pre-baked 256×256 BGRA atlas — use `:token:` syntax
- On-screen keyboard for controller-only setups (Y button)
- USB keyboard support via RXDK debug keyboard path
- Password protected rooms — prompted on join via on-screen keyboard or USB keyboard
- ACL rooms — hidden from users who lack the required role (server enforced)
- Online users overlay (R3) — shows all connected users and their current room
- Analog stick navigation and cursor control
- 480p display output

---

## Source Files

| File | Purpose |
|------|---------|
| `main.cpp` | Entry point, state machine |
| `auth.cpp / auth.h` | Login, register, auto-login, version display |
| `chat.cpp / chat.h` | Chat UI, room list, message rendering, word wrap |
| `sc_net.cpp / sc_net.h` | Network state machine, ChaCha20-Poly1305 |
| `sc_dh.cpp / sc_dh.h` | DH key exchange, bignum |
| `sc_log.cpp / sc_log.h` | File logger (`D:\scenechat.log`) |
| `creds.cpp / creds.h` | Credential persistence (`D:\creds.dat`) |
| `emoji.cpp / emoji.h` | Emoji atlas rendering |
| `emoji_atlas.h` | Auto-generated atlas data |
| `debug_keyboard.cpp / .h` | RXDK USB keyboard backend |
| `hid.cpp / hid.h` | HID adapter layer |
| `sc_textinput.cpp / .h` | Input buffer abstraction |
| `osk.cpp / osk.h` | On-screen keyboard |
| `font.cpp / font.h` | Bitmap font renderer |
| `ui.cpp / ui.h` | UI layout, draw primitives |
| `voice.cpp / voice.h` | Voice chat (UDP port 7800) |
| `input.cpp / input.h` | Controller input, XInitDevices |

---

## Notes

- `D:\creds.dat` stores the username and session token in plaintext. Physical access to the Xbox drive exposes these credentials.
- Log output is written to `D:\scenechat.log` for debugging.
- Room access is enforced server-side. Rooms not in your list are hidden by the server based on your role. Rooms marked with `*` require a password.
- The on-screen keyboard is controller-navigable. If a USB keyboard is connected, type directly in the input bar at any time.