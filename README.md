# SceneChat

<div align=center>

<img src="https://github.com/Darkone83/SceneChat/blob/main/img/Chat.png" width=400> <img src="https://github.com/Darkone83/SceneChat/blob/main/img/scenechat.png" width=400>

</div>

<div align=center>

<img src="https://github.com/Darkone83/SceneChat/blob/main/img/Darkone83.png">

</div>

**A private encrypted chat client for the original Xbox, built by Team Resurgent / Darkone83.**

SceneChat is a from-scratch homebrew chat application for the original Microsoft Xbox (2001). It implements a custom encrypted network protocol, real-time multi-room messaging, private direct messages, an offline mailbox, USB keyboard support, inline emoji rendering, over-the-air client updates, and a web-based admin panel — all running natively on Xbox hardware via the RXDK SDK.

This is not a port of an existing chat application. Every layer — from the bignum DH implementation to the Poly1305 MAC to the D3D8 font renderer — was written specifically for 733MHz Xbox hardware.

Desktop clients for Windows (WPF) and any platform (Python) connect to the same server using the identical SCCP protocol.

> ⚠️ **Private Use Notice** — SceneChat is designed for use on private, trusted networks only. It is not hardened for public internet deployment. See [SECURITY.md](SECURITY.md) for a full breakdown of the security model and known limitations before deploying.

---

## Features

### Client (Xbox) — v1.3
- Custom encrypted protocol (1024-bit DH + ChaCha20-Poly1305 + HKDF)
- User registration and login with bcrypt-hashed credentials
- Auto-login via saved credentials on the Xbox hard drive
- Multi-room chat with real-time message broadcast
- **Direct messages** — private one-to-one conversations, opened from the online users overlay
- **Offline mailbox** — send and receive messages that are delivered on next login
- **Over-the-air updates** — the client checks the server version on login and can download and apply a new release as an XBA package, then relaunch
- Message word wrap and newline rendering
- Real-time deleted message sync — admin deletions reflect on-screen instantly
- USB keyboard input via RXDK debug keyboard path
- On-screen keyboard (OSK) for controllers-only setups
- Inline emoji rendering via a pre-baked 256x256 BGRA atlas
- Analog stick navigation and D-Pad cursor control
- Password protected and access-controlled rooms
- Back button returns to login screen without disconnecting uncleanly
- Version number displayed on login/connecting screen
- 480p display output targeting original Xbox hardware

### Desktop Clients — v1.3
- **Windows (WPF)** — native .NET 8 desktop client
- **Python (PySide6)** — cross-platform desktop client
- Both implement the full encrypted protocol, multi-room chat, DMs, mailbox, emoji, and password/ACL rooms
- DMs and mailbox open in dedicated windows for desktop flexibility

### Server — v1.3
- Python asyncio TCP server on port 8943
- Per-connection DH key exchange with fresh session keys
- ChaCha20-Poly1305 encryption on every packet
- MySQL/MariaDB backend for users, rooms, message history, DM participants, and mailbox
- bcrypt password hashing (rounds=12) with per-user salt
- Room management (text and voice room types)
- **Direct message rooms** — private rooms between two users, delivered to both regardless of active room
- **Mailbox** — offline message storage and delivery on login, including send-by-username for offline recipients
- **OTA update hosting** — version string and XBA package served to Xbox clients
- Helper bot (`scene_bot`) — `/help`, `/online`, `/rooms`, `/emoji`, `/kick`, `/ban`, `/mute`, `/announce` and more
- Admin message broadcast to live connected clients
- Real-time deleted message broadcast to connected clients
- Online presence persistence — last seen and last room per user
- Rotating log files with logrotate config included
- Internal admin API on localhost:8951 (not externally accessible)

### Admin Panel — v1.3
- Flask web application on port 8950
- Role-based access: superadmin / admin / moderator
- Live chat monitor with per-room message view and real-time delete broadcast
- Admin can send messages to any room with live broadcast to all clients
- Emoji picker with all 33 SceneChat emoji
- Message deletion with real-time sync to connected clients
- User management: ban, unban, delete, role assignment, last seen, last room
- Room creation and deletion
- **Mailbox** — inbox with mark-read, compose, and full audit view
- **DM management** — metadata-only view of DM channels (participants, message counts, activity) for moderation; message content is never exposed
- **Update hosting** — host the XBA package and set the client version string
- Log viewer with level filtering, text search, auto-scroll, and auto-refresh
- Maintenance page: DB health stats, purge tools, backup, download, restore

---

## Privacy

Direct messages are private. DM rooms are excluded from the public room list and are walled off from the admin panel — the only DM data an admin can see is channel metadata (who is talking to whom, message counts, activity times) for moderation purposes. **Message content is never accessible to administrators.**

---

## Emoji

All emoji are referenced inline using `:token:` syntax, identical across the Xbox, Python, and WPF clients and the admin panel.

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

See [SECURITY.md](SECURITY.md) for the full security model and known limitations.


---

## Requirements

### Xbox Client
- Original Xbox with modchip or softmod
- RXDK (Repackaged Xbox Development Kit)
- MSVC 2003 / Xbox SDK toolchain
- USB keyboard optional (RXDK debug keyboard path)

### Windows Client
- Windows 10 or 11
- .NET 8 SDK (for building)

### Python Client
- Python 3.10+
- Dependencies install automatically on first run (`PySide6`, `cryptography`)

### Server
- Python 3.10+
- MySQL 5.7+ or MariaDB 10.3+
- `pip install mysql-connector-python bcrypt cryptography flask requests pillow`
- `mysqldump` and `mysql` CLI tools for backup/restore feature

### Admin Panel
- Accessible via browser on port 8950

---

## Credits

**Team Resurgent / Darkone83**

Built as part of the original Xbox homebrew and preservation community.