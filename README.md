# SceneChat

**A private encrypted chat client for the original Xbox, built by Team Resurgent / Darkone83.**

SceneChat is a from-scratch homebrew chat application for the original Microsoft Xbox (2001). It implements a custom encrypted network protocol, real-time multi-room messaging, USB keyboard support, inline emoji rendering, and a web-based admin panel — all running natively on Xbox hardware via the RXDK SDK.

This is not a port of an existing chat application. Every layer — from the bignum DH implementation to the Poly1305 MAC to the D3D8 font renderer — was written specifically for 733MHz Xbox hardware.

---

## Features

### Client (Xbox)
- Custom encrypted protocol (1024-bit DH + ChaCha20-Poly1305 + HKDF)
- User registration and login with bcrypt-hashed credentials
- Auto-login via saved credentials on the Xbox hard drive
- Multi-room chat with real-time message broadcast
- USB keyboard input via RXDK debug keyboard path
- On-screen keyboard (OSK) for controllers-only setups
- Inline emoji rendering via a pre-baked 256x256 BGRA atlas
- Analog stick navigation and D-Pad cursor control
- Back button returns to login screen without disconnecting uncleanly
- 480p display output targeting original Xbox hardware

### Server
- Python asyncio TCP server on port 8943
- Per-connection DH key exchange with fresh session keys
- ChaCha20-Poly1305 encryption on every packet
- MySQL backend for users, rooms, and message history
- bcrypt password hashing with per-user salt
- Room management (text and voice room types)
- Admin message broadcast to live connected clients
- Internal admin API on localhost:8951

### Admin Panel
- Flask web application on port 8950
- Role-based access: superadmin / admin / moderator
- Live chat monitor with 2-second polling per room
- Admin can send messages to any room with live broadcast to Xbox clients
- Emoji picker with all 33 SceneChat emoji
- Message deletion
- User management: ban, unban, delete, role assignment
- Room creation and deletion

---

## Emoji

All emoji are referenced inline using `:token:` syntax, identical between the Xbox client and the admin panel.

```
Faces:
:smile: :wink: :laugh: :cry: :angry: :sad: :surprised: :thinking:
:cool: :love_face: :dead: :party: :question:

Reactions:
:thumbs_up: :thumbs_down: :check: :x: :alert:

Objects:
:heart: :fireheart: :star: :skull: :fire:

SceneChat Originals:
:scenechat_sc: :scenechat_softmod: :scenechat_fire: :scenechat_modchip:
:scenechat_controller_chat: :scenechat_ping: :scenechat_lobby: :scenechat_dpad:
:scenechat_buttons: :scenechat_devbuild:
```

---

## Security Overview

- **DH key exchange** — fresh 1024-bit DH per connection, no session key reuse
- **HKDF derivation** — shared secret derived with info string `scenechat-session`
- **ChaCha20-Poly1305** — all post-handshake traffic encrypted and authenticated
- **bcrypt** — passwords hashed with bcrypt before storage, never stored plaintext
- **Token auth** — 64-byte random hex session token for auto-login
- **No server verification** — no certificate pinning; designed for a trusted private network

---

## Project Structure

```
SceneChat/
├── client/                  # Xbox RXDK C++ source
│   ├── main.cpp             # Entry point, state machine
│   ├── auth.cpp / auth.h    # Login, register, auto-login
│   ├── chat.cpp / chat.h    # Chat UI, room list, message rendering
│   ├── sc_net.cpp / sc_net.h        # Network state machine, ChaCha20-Poly1305
│   ├── sc_dh.cpp / sc_dh.h         # DH key exchange, bignum
│   ├── sc_log.cpp / sc_log.h       # File logger (D:\scenechat.log)
│   ├── creds.cpp / creds.h         # Credential persistence (D:\creds.dat)
│   ├── emoji.cpp / emoji.h         # Emoji atlas rendering
│   ├── emoji_atlas.h               # Auto-generated atlas data
│   ├── debug_keyboard.cpp / .h     # RXDK USB keyboard backend
│   ├── hid.cpp / hid.h             # HID adapter layer
│   ├── sc_textinput.cpp / .h       # Input buffer abstraction
│   ├── osk.cpp / osk.h             # On-screen keyboard
│   ├── font.cpp / font.h           # Bitmap font renderer
│   ├── ui.cpp / ui.h               # UI layout, draw primitives
│   └── input.cpp / input.h         # Controller input, XInitDevices
│
├── server/
│   ├── server.py            # Main asyncio chat server (port 8943)
│   ├── admin.py             # Flask admin panel (port 8950)
│   └── templates/
│       ├── base.html
│       ├── login.html
│       ├── dashboard.html
│       ├── monitor.html
│       ├── users.html
│       └── rooms.html
│
└── tools/
    └── gen_emoji.py         # Emoji PNG → BGRA atlas header generator
```

---

## Requirements

### Xbox Client
- Original Xbox with modchip or softmod
- RXDK (Repackaged Xbox Development Kit)
- MSVC 2003 / Xbox SDK toolchain
- USB keyboard optional (RXDK debug keyboard path)

### Server
- Python 3.8+
- MySQL 5.7+ or MariaDB
- `pip install asyncio mysql-connector-python bcrypt cryptography requests pillow`

### Admin Panel
- Accessible via browser on port 8950
- `requests` required for admin→server broadcast bridge

---

## Credits

**Team Resurgent / Darkone83**

Built as part of the original Xbox homebrew and preservation community.