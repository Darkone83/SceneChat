# SceneChat — Xbox Client

Native original Xbox homebrew chat client for SceneChat. Built with the RXDK SDK targeting 733MHz Xbox hardware.

**Team Resurgent / Darkone83 — v1.3**

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
| D-pad up/down | Scroll messages (chat focus) / select room or DM (sidebar focus) |
| D-pad left/right | Switch focus between sidebar and chat |
| A / Left Trigger | Join selected room or DM |
| White | Toggle sidebar between ROOMS and DMs |
| Black | Toggle the mailbox overlay |
| Y | Open on-screen keyboard |
| Start | Send message |
| B | Clear input / close overlay |
| R3 (right stick click) | Toggle online users overlay |
| Back | Return to login screen |

**Online users overlay (R3):**
- D-pad up/down to select a user
- A — open a DM with the selected user
- X — compose mail to the selected user
- B — close

**Mailbox overlay (Black):**
- D-pad up/down to select mail
- A — read, X — delete, Y — compose, B — close
- In compose: up/down switches To/Message fields, A opens the keyboard for the focused field, Start sends, B cancels

**USB keyboard:**
- Type directly into the input bar (or the focused compose field) at any time
- Tab switches between compose fields, Enter sends, Escape cancels

---

## Features

- Full DH + ChaCha20-Poly1305 encrypted connection
- User registration and login
- Auto-login via saved credentials on the Xbox hard drive (`D:\creds.dat`)
- Multi-room chat with real-time messages
- **Direct messages** — open from the online users overlay; DM list in the sidebar (toggle with White)
- **Offline mailbox** — read, compose, reply, and delete; mail delivered on login
- **Over-the-air updates** — on login the client compares its version to the server and, if newer, can download an XBA package over HTTP, extract it, and relaunch automatically
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
| `auth.cpp / auth.h` | Login, register, auto-login, update check/prompt, version display |
| `chat.cpp / chat.h` | Chat UI, room list, DM list, mailbox overlay, message rendering, word wrap |
| `sc_net.cpp / sc_net.h` | Network state machine, ChaCha20-Poly1305, DM and mail protocol |
| `sc_update.cpp / sc_update.h` | OTA update — version check, XBA download, extract, relaunch |
| `xba.cpp / xba.h` | XBA archive extraction |
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

## Over-the-Air Updates

On login the server sends the current published version. The client compares it against its own compiled version constant. If the server version is newer, a prompt appears:

1. **A** — download and apply the update
2. **B** — skip and continue to chat

When applying, the client downloads `scenechat.xba` over HTTP from the admin panel, extracts it to the install directory (XBE first, `.ver` last), then relaunches the new XBE via `XLaunchNewImage`. The download uses blocking sockets with send/receive timeouts, mirroring the XbDiag update architecture.

---

## Notes

- `D:\creds.dat` stores the username and session token in plaintext. Physical access to the Xbox drive exposes these credentials.
- Log output is written to `D:\scenechat.log` for debugging.
- Room access is enforced server-side. Rooms not in your list are hidden by the server based on your role. Rooms marked with `*` require a password.
- DM rooms are private — they never appear in the public room list and are not readable by administrators.
- The on-screen keyboard is controller-navigable. If a USB keyboard is connected, type directly in the input bar at any time.