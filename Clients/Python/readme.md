# SceneChat — Python Client

A PySide6 desktop client for SceneChat. Connects to the same server as the Xbox and WPF clients using the identical SCCP protocol.

**Team Resurgent / Darkone83 — v1.3**

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
| `sc_net.py` | Network worker — DH handshake, encryption, SCCP protocol (chat, DMs, mailbox) |
| `sc_ui.py` | PySide6 UI — login, room list, DM list, chat, mailbox window, compose window, emoji picker |
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
- **Direct messages** — right-click a user in the online panel (or double-click) to open a DM; active DMs appear in a dedicated sidebar section
- **Mailbox** — a standalone window: read, mark-read, reply, delete, and compose; mail is delivered on login
- **Compose to anyone** — send mail to a known user or type a username to mail an offline user
- Real-time deleted message sync — messages deleted by an admin disappear instantly
- Inline emoji rendering — type `:smile:` `:fire:` etc.
- Emoji picker (click 😊 in the input bar)
- Password protected rooms — prompted on join
- ACL rooms — only visible to users with sufficient role
- Online users panel — live list of connected users and their current room, updates in real time
- Auto-login from saved credentials
- Logout clears saved credentials and resets DM/mail state

---

## Direct Messages and Mailbox

**DMs** open in the main chat view, just like a room. Open one by right-clicking a user in the online panel (Open DM) or double-clicking them. Active DM channels appear in the DIRECT MESSAGES section of the sidebar — click one to return to that conversation. DMs are delivered in real time to both participants.

**Mailbox** is for offline messaging. Click **Mailbox** in the top bar to open the mailbox window. New mail is delivered when you log in. Compose mail to a known user via the online panel right-click menu (Send Mail), or use Compose in the mailbox window and type any username — the server resolves it, so you can mail users who are currently offline.

---

## Notes

Emoji are downloaded from the admin panel on port 8950 of your server. If the admin panel is not running or the port is blocked, emoji will render as `:token:` text instead of images — chat still works normally.

Room access is enforced server-side. If you cannot see a room, you do not have the required role. If a room shows `*` it requires a password to join. DM rooms are private and never appear in the public room list.