# SceneChat — Python Client

A PySide6 desktop client for SceneChat. Connects to the same server as the Xbox and WPF clients using the identical SCCP protocol.

**Team Resurgent / Darkone83**

---

## Requirements

- Python 3.10 or newer
- That's it — all other dependencies install automatically on first run

---

## Running

**Windows** — double-click `run.bat`, or:
```
python SceneChat.py
```

**Linux / macOS** — double-click `run.sh`, or:
```
./run.sh
```

On first launch, `PySide6` and `cryptography` will be installed automatically. This takes about a minute. Subsequent launches start instantly.

---

## Files

| File | Purpose |
|------|---------|
| `SceneChat.py` | Entry point and dependency installer |
| `sc_net.py` | Network worker — DH handshake, encryption, SCCP protocol |
| `sc_ui.py` | PySide6 UI — login, room list, chat, emoji picker |
| `run.bat` | Windows launcher |
| `run.sh` | Linux / macOS launcher |

Two files are created on first use:
- `creds.json` — saved server address, username, and password for auto-login
- `emoji_cache/` — emoji PNGs downloaded from the server on first connect

---

## Features

- Full DH + ChaCha20-Poly1305 encrypted connection — same security as the Xbox client
- Login and registration
- Multi-room chat with real-time messages
- Real-time deleted message sync — messages deleted by an admin disappear instantly
- Inline emoji rendering — type `:smile:` `:fire:` etc.
- Emoji picker (click 😊 in the input bar)
- Password protected rooms — prompted on join
- ACL rooms — only visible to users with sufficient role
- Online users panel — live list of connected users and their current room, updates in real time
- Auto-login from saved credentials
- Logout clears saved credentials

---

## Notes

Emoji are downloaded from the admin panel on port 8950 of your server. If the admin panel is not running or the port is blocked, emoji will render as `:token:` text instead of images — chat still works normally.

Room access is enforced server-side. If you cannot see a room, you do not have the required role. If a room shows `*` it requires a password to join.