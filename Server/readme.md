# SceneChat Server Documentation

**Team Resurgent / Darkone83 — v1.3**

This document covers the full server-side architecture of SceneChat — the chat server, voice server, admin panel, database schema (including DMs and mailbox), the OTA update hosting, and all HTML templates.

---

## Table of Contents

1. [Overview](#overview)
2. [Directory Structure](#directory-structure)
3. [Database Schema](#database-schema)
4. [Chat Server — server.py](#chat-server--serverpy)
5. [Voice Server — voice_server.py](#voice-server--voice_serverpy)
6. [Admin Panel — admin.py](#admin-panel--adminpy)
7. [Template Reference](#template-reference)
8. [Ports and Services](#ports-and-services)
9. [Dependencies](#dependencies)

---

## Overview

The SceneChat backend consists of three independent Python processes:

| Process | File | Port | Protocol |
|---------|------|------|----------|
| Chat server | `server.py` | 8943 | TCP (SCCP encrypted binary) |
| Voice server | `voice_server.py` | 7800 | UDP (ADPCM relay) |
| Admin panel | `admin.py` | 8950 | HTTP (Flask) |
| Admin API bridge | `server.py` (internal) | 8951 | HTTP (localhost only) |

All three processes share the same MySQL database. The chat server and admin panel communicate via the internal HTTP bridge on port 8951 — this port is bound to `127.0.0.1` only and is never exposed externally.

---

## Directory Structure

```
/opt/scenechat/
├── server.py               # Chat server (asyncio TCP)
├── voice_server.py         # Voice relay server (asyncio UDP)
├── admin.py                # Admin web panel (Flask)
├── scenechat.logrotate     # logrotate config (install to /etc/logrotate.d/)
├── emoji/                  # Emoji PNG files (32x32)
│   ├── smile.png
│   ├── angry.png
│   └── ...                 # 33 emoji total
├── logs/                   # Server log files (auto-created)
│   └── scenechat.log       # Rotating log, 50MB per file, 30 file retention
├── backups/                # DB backups from admin panel (auto-created)
├── update/                 # OTA update files served to Xbox clients
│   ├── scenechat.ver       # plain-text version string
│   └── scenechat.xba       # packaged client release
└── templates/              # Jinja2 HTML templates
    ├── base.html
    ├── login.html
    ├── dashboard.html
    ├── monitor.html
    ├── users.html
    ├── rooms.html
    ├── dms.html
    ├── mailbox.html
    ├── update.html
    ├── logs.html
    └── maintenance.html
```

---

## Database Schema

All three services connect to the same MySQL database using the `scenechat` user.

### Connection config (shared across all services)

```python
DB_CONFIG = {
    'host':     'localhost',
    'user':     'scenechat',
    'password': 'your_password_here',   # set a strong unique password
    'database': 'scenechat'
}
```

### `users` table

| Column | Type | Description |
|--------|------|-------------|
| `id` | INT AUTO_INCREMENT PK | User ID |
| `username` | VARCHAR | Unique username |
| `password_hash` | VARCHAR | bcrypt hash (rounds=12) |
| `salt` | VARCHAR | bcrypt salt |
| `role` | ENUM | `user`, `moderator`, `admin` |
| `is_banned` | TINYINT | 0=active, 1=banned |
| `token` | VARCHAR(128) | Session token (hex) |
| `token_expiry` | DATETIME | Token expiry timestamp |
| `created_at` | DATETIME | Registration timestamp |
| `last_seen` | DATETIME | Last clean disconnect timestamp (NULL if never) |
| `last_room` | INT | Room ID at last disconnect |

### `rooms` table

| Column | Type | Description |
|--------|------|-------------|
| `id` | INT AUTO_INCREMENT PK | Room ID |
| `name` | VARCHAR | Room name (DM rooms use an internal `dm_x_y` name) |
| `type` | ENUM | `text`, `voice`, `dm` |
| `password_hash` | VARCHAR(128) | bcrypt hash; NULL = no password |
| `access_level` | ENUM | `public`, `moderator`, `admin` |
| `created_at` | DATETIME | Creation timestamp |

### `room_participants` table

Tracks the two members of each DM room. One row per participant.

| Column | Type | Description |
|--------|------|-------------|
| `room_id` | INT FK | References a `rooms` row with `type = 'dm'` |
| `user_id` | INT FK | One of the two participants |
| `joined_at` | DATETIME | When the participant was added |

### `mailbox` table

Offline messages, delivered to the recipient on next login.

| Column | Type | Description |
|--------|------|-------------|
| `id` | INT AUTO_INCREMENT PK | Mail ID |
| `sender_id` | INT FK | References `users.id` |
| `recipient_id` | INT FK | References `users.id` |
| `content` | TEXT | Message body |
| `sent_at` | DATETIME | Timestamp |
| `is_read` | TINYINT | 0=unread, 1=read |
| `is_deleted` | TINYINT | 0=visible, 1=soft-deleted |

### `messages` table

| Column | Type | Description |
|--------|------|-------------|
| `id` | INT AUTO_INCREMENT PK | Message ID |
| `room_id` | INT FK | References `rooms.id` |
| `user_id` | INT FK | References `users.id` |
| `content` | TEXT | Message content (may contain `:token:` emoji) |
| `sent_at` | DATETIME | Timestamp |
| `is_deleted` | TINYINT | 0=visible, 1=soft-deleted |

---

## Chat Server — server.py

### Responsibilities

- Accepts Xbox client TCP connections on port 8943
- Performs DH key exchange per connection
- Encrypts/decrypts all traffic with ChaCha20-Poly1305
- Authenticates users (register and login) against the database
- Maintains connected client state in memory
- Broadcasts messages to all clients in a room
- Serves message history on room join
- Handles admin message injection via internal HTTP API on port 8951

### Connected client state

Each connected client is tracked in the `connected_clients` dict:

```python
connected_clients = {
    user_id: {
        'writer':      asyncio.StreamWriter,   # TCP write handle
        'lock':        asyncio.Lock,           # per-client write lock
        'session_key': bytes,                  # 32-byte ChaCha20 key
        'username':    str,
        'room':        int,                    # current room_id or None
    }
}
```

### Packet handler flow

Every encrypted packet received from a client goes through:

```
read_encrypted()
    -> decrypt ChaCha20-Poly1305
    -> verify Poly1305 tag (drop connection on failure)
    -> route by pkt_type to handler

0x03 REGISTER     -> validate username/password, bcrypt hash, insert user, send AUTH_OK/FAIL
0x04 LOGIN        -> lookup user, bcrypt verify, update token, send AUTH_OK/FAIL,
                     then deliver existing DM rooms and any unread mail
0x07 ROOM_LIST    -> send public room list (DM rooms always excluded)
0x08 JOIN_ROOM    -> update client['room'], send ROOM_INFO + HISTORY
0x0A MESSAGE      -> validate, insert to DB, broadcast MSG_RECV (DM = participants only)
0x0E PING         -> send PONG (unencrypted)
0x10 DISCONNECT   -> update last_seen + last_room, clean up connected_clients
0x14 DM_OPEN      -> find or create DM room between two users, send ROOM_INFO to both
0x15 MAIL_LIST    -> (S->C) deliver mail list on login or after a new mail arrives
0x16 MAIL_SEND    -> store mail; resolve recipient by id, or by name (TO:name) if offline
0x17 MAIL_READ    -> mark a mail item read
0x18 MAIL_DELETE  -> soft-delete a mail item
```

### Broadcast

When a message is received (or injected by admin), the server iterates `connected_clients` and writes to every client in the same room:

```python
for uid, client in list(connected_clients.items()):
    if client.get('room') == room_id:
        await write_encrypted(
            client['writer'], client['lock'],
            client['session_key'],
            SCCP_MSG_RECV, payload
        )
```

Each write is guarded by a per-client `asyncio.Lock` to prevent concurrent write corruption on the same TCP stream.

For **DM rooms** the broadcast is restricted: instead of every client whose active room matches, the server delivers to every online DM participant regardless of the room they currently have focused. This ensures both sides of a DM receive the message even if one is viewing a different room.

### Direct messages

A DM is a `rooms` row with `type = 'dm'` plus two `room_participants` rows. On `DM_OPEN` the server:

1. Verifies the target user exists and is not the caller
2. Looks for an existing DM room shared by both users; creates one if none exists
3. Sends a `ROOM_INFO` (with `room_type = 2`) to the requester, and to the target if online

The `ROOM_INFO` display name for a DM is `DM:<other-username>` so each side sees the other participant's name rather than the internal `dm_x_y` room name. DM rooms are never included in the public `ROOM_LIST`. On login the server also pushes any existing DM rooms the user participates in.

### Mailbox

Offline messaging backed by the `mailbox` table. On login the server delivers any unread mail via `MAIL_LIST`. `MAIL_SEND` accepts either a numeric `recipient_id`, or `recipient_id = 0` with the body prefixed `TO:<username>\n` — the server resolves the username, which lets clients mail users who are currently offline (and whose id they do not know). If the recipient is online, the new mail is delivered immediately via `MAIL_LIST`; otherwise it waits for their next login. `MAIL_READ` and `MAIL_DELETE` update the read/deleted flags.

### Admin API bridge (port 8951)

An internal asyncio HTTP server runs alongside the main chat server, bound to `127.0.0.1:8951` only. Flask POSTs to this endpoint to inject admin messages:

```
POST /admin/send
Content-Type: application/json

{"room_id": 1, "content": "Hello from admin :smile:"}
```

```
POST /admin/delete
Content-Type: application/json

{"room_id": 1, "msg_id": 42}
```

The `/admin/delete` endpoint broadcasts `SCCP_MSG_DELETE (0x19)` to all clients in the room, causing connected clients to replace the message content with `[deleted]` in real time.

The handler calls `admin_broadcast()` which:
1. Looks up or creates the reserved `Admin` user in the database
2. Inserts the message into the `messages` table
3. Broadcasts to all connected clients in the room via `write_encrypted`

Both servers (8943 and 8951) run concurrently under the same asyncio event loop:

```python
async with server:
    async with admin_server:
        await asyncio.gather(
            server.serve_forever(),
            admin_server.serve_forever()
        )
```

### Logging

Server uses Python's `RotatingFileHandler` — 50MB per file, 30 files retained:

```python
_log_handler = logging.handlers.RotatingFileHandler(
    '/opt/scenechat/logs/scenechat.log',
    maxBytes=50 * 1024 * 1024,
    backupCount=30,
    encoding='utf-8'
)
```

Log also writes to stdout for systemd journal capture. Install `scenechat.logrotate` to `/etc/logrotate.d/scenechat` for daily rotation and 30-day compressed retention:

```bash
sudo cp scenechat.logrotate /etc/logrotate.d/scenechat
sudo logrotate -d /etc/logrotate.d/scenechat  # dry run
```

---

### DH parameter generation

On startup the server generates fresh 1024-bit DH parameters:

```python
parameters = dh.generate_parameters(generator=2, key_size=1024, backend=default_backend())
```

These are reused for all connections until the server restarts. Each connection generates a fresh private/public key pair from these parameters.

### String encoding (SCCP wire format)

```python
def pack_string8(s):   # 1-byte length + UTF-8 (max 255 bytes)
def pack_string16(s):  # 2-byte length + UTF-8 (max 65535 bytes)
def unpack_string8(data, pos):
def unpack_string16(data, pos):
```

All multi-byte integers are big-endian.

---

## Voice Server — voice_server.py

### Responsibilities

- Receives UDP ADPCM audio packets from Xbox clients on port 7800
- Validates connecting users against the database (token_expiry check)
- Relays audio to all other clients in the same voice room
- Evicts stale clients after 10 seconds of inactivity (checked every 30 seconds)

### Packet format

**Join packet** (empty payload — triggers registration):
```
[4 bytes] user_id (big-endian uint32)
[1 byte]  room_id
[1 byte]  slot (mixer slot index, 0–3)
```

**Audio packet** (non-empty payload):
```
[4 bytes] user_id (big-endian uint32)
[1 byte]  room_id
[1 byte]  slot
[N bytes] ADPCM audio payload (max ~1018 bytes)
```

**Join acknowledgement** (server → client):
```
[1 byte] 0xFF
```

**Relay packet** (server → other clients in room):
```
[1 byte]  slot (sender's slot)
[N bytes] ADPCM payload
```

### Client state

```python
voice_clients = {
    user_id: {
        'addr':      (ip, port),   # UDP return address
        'room_id':   int,
        'slot':      int,
        'last_seen': datetime,
    }
}

voice_rooms = {
    room_id: set()   # set of user_ids in the room
}
```

### User validation

On join, the voice server validates the connecting `user_id` against the database:

```python
SELECT id FROM users
WHERE id = %s AND is_banned = 0 AND token_expiry > NOW()
```

Clients that fail validation are silently dropped — no error is sent back.

### Stale client cleanup

A background coroutine runs every 30 seconds and removes any client whose `last_seen` is more than 10 seconds ago. This handles clients that disconnect without sending a leave packet (common on Xbox reboot/crash).

### Security note

The voice server does **not** encrypt audio traffic. UDP packet ordering and XBox memory constraints make full AEAD impractical for real-time audio. The voice channel is integrity-protected only by the user_id validation on join — a client that passes validation is trusted for the duration of the session.

---

## Admin Panel — admin.py

### Responsibilities

- Web interface for server administration (port 8950)
- Role-based access control (superadmin / admin / moderator)
- Live chat monitoring with 2-second message polling
- Admin message sending with live broadcast to Xbox clients
- User management (ban, unban, delete, role assignment)
- Room management (create, delete)
- Emoji file serving for the monitor

### Authentication

Two authentication paths:

**Superadmin** — hardcoded bcrypt hash in source, never stored in database:
```python
SUPERADMIN_USERNAME      = 'admin'
SUPERADMIN_PASSWORD_HASH = '$2b$12$...'   # bcrypt rounds=12
```

**Admin/Moderator users** — bcrypt hash in `users` table, role must be `admin` or `moderator`. Banned users cannot log in regardless of role.

Flask session stores:
```python
session['admin_logged_in'] = True
session['role']             = 'superadmin' | 'admin' | 'moderator'
session['display_name']     = str
session['user_id']          = int   # not set for superadmin
```

### Role permissions

| Action | Moderator | Admin | Superadmin |
|--------|-----------|-------|------------|
| View dashboard | ✓ | ✓ | ✓ |
| Monitor chat | ✓ | ✓ | ✓ |
| Delete messages | ✓ | ✓ | ✓ |
| Ban/unban users | ✓ | ✓ | ✓ |
| Send admin messages | ✓ | ✓ | ✓ |
| Assign roles | ✗ | ✓ | ✓ |
| Delete users | ✗ | ✓ | ✓ |
| Create rooms | ✗ | ✗ | ✓ |
| Delete rooms | ✗ | ✗ | ✓ |
| DM management (metadata only) | ✗ | ✓ | ✓ |
| Mailbox (inbox / compose / audit) | ✓ | ✓ | ✓ |
| Update hosting (XBA / version) | ✗ | ✓ | ✓ |
| View logs | ✓ | ✓ | ✓ |
| DB maintenance / backup / restore | ✓ | ✓ | ✓ |

### Routes

| Method | Route | Auth | Description |
|--------|-------|------|-------------|
| GET | `/` | — | Redirect to dashboard or login |
| GET/POST | `/login` | — | Admin login form |
| GET | `/logout` | Any | Clear session |
| GET | `/dashboard` | Moderator | Stats overview |
| GET | `/monitor` | Moderator | Live chat monitor |
| GET | `/api/messages/<room_id>` | Moderator | Poll messages (JSON; returns empty for DM rooms) |
| POST | `/api/send_message` | Moderator | Send admin message |
| GET | `/messages/delete/<id>` | Moderator | Soft-delete a message |
| GET | `/users` | Moderator | User list |
| GET | `/users/ban/<id>` | Moderator | Ban user |
| GET | `/users/unban/<id>` | Moderator | Unban user |
| POST | `/users/role/<id>` | Admin | Set user role |
| GET | `/users/delete/<id>` | Admin | Delete user + messages |
| GET | `/rooms` | Superadmin | Room list |
| POST | `/rooms/create` | Superadmin | Create room |
| GET | `/rooms/delete/<id>` | Superadmin | Delete room + messages (refuses DM rooms) |
| GET | `/dms` | Admin | DM channel list (metadata only, no content) |
| POST | `/dms/delete/<id>` | Admin | Remove a DM channel (moderation) |
| GET | `/mailbox` | Moderator | Inbox, compose, and all-mail audit |
| POST | `/mailbox/send` | Moderator | Send mail to a username |
| POST | `/mailbox/read/<id>` | Moderator | Mark own mail read |
| POST | `/mailbox/read_all` | Moderator | Mark all own mail read |
| POST | `/mailbox/delete/<id>` | Moderator | Soft-delete a mail item |
| GET | `/update_mgmt` | Admin | Update hosting page (current version, upload) |
| POST | `/update/upload` | Admin | Upload XBA package and set version string |
| GET | `/update/` and `/update/<file>` | — | Serve update files (no auth; Xbox pre-login) |
| GET | `/emoji/<name>.png` | — | Serve emoji PNG from `/opt/scenechat/emoji/` |
| GET | `/logs` | Moderator | Log viewer page |
| GET | `/api/logs` | Moderator | Fetch log lines (JSON, ?lines=N) |
| GET | `/maintenance` | Moderator | DB health, purge, backup, restore |
| POST | `/maintenance/backup` | Moderator | Create DB backup via mysqldump |
| GET | `/maintenance/backup/download/<file>` | Moderator | Download backup file |
| POST | `/maintenance/backup/delete/<file>` | Moderator | Delete backup file |
| POST | `/maintenance/restore` | Moderator | Restore DB from uploaded .sql file |
| POST | `/maintenance/purge/deleted` | Moderator | Hard-delete soft-deleted messages |
| POST | `/maintenance/purge/old` | Moderator | Delete messages older than N days |
| POST | `/maintenance/purge/inactive` | Moderator | Delete inactive user accounts |

### Message polling (monitor)

The monitor page polls `/api/messages/<room_id>?since=<last_id>` every 2 seconds. The API returns up to 100 messages with IDs greater than `since_id`, ordered by `sent_at ASC`. The client tracks the highest seen message ID and only fetches new messages each poll.

### Admin send flow

```
Browser POST /api/send_message
    -> admin.py validates session
    -> HTTP POST to http://127.0.0.1:8951/admin/send
        -> server.py _handle_admin_http()
            -> admin_broadcast(room_id, content)
                -> INSERT into messages table
                -> write_encrypted() to all clients in room
    -> JSON response {"ok": true, "msg": "Sent to N clients"}
    -> Monitor shows message in feed on next poll
```

---

## Template Reference

All templates extend `base.html` using Jinja2 `{% extends %}` / `{% block content %}`.

### base.html

The master layout. Provides:

- Full dark theme CSS (background `#0a0a0a`, accent `#39ff14`, secondary `#8b5cf6`)
- Navbar with SceneChat logo, navigation links, logged-in username, and logout
- Navigation links are conditionally shown by role — Rooms and DMs tabs only visible to admin/superadmin
- Active page highlighting via `request.endpoint` comparison
- Flash message display block
- Footer with Team Resurgent / Darkone83 branding
- Shared CSS classes: `.card`, `.btn`, `.btn-green`, `.btn-red`, `.btn-purple`, `.btn-gray`, `.badge`, `.badge-green`, `.badge-red`, `.badge-purple`, `.badge-blue`, `.stats-grid`, `.stat-card`, `.form-row`, `.flash`

**Variables used:** `session.admin_logged_in`, `session.role`, `session.display_name`

---

### login.html

Simple centered login form. No navbar (user is not authenticated). Displays the SceneChat logo above the form card.

**Template variables:** none

**Form fields:** `username` (text), `password` (password)

**Submits to:** `POST /login`

---

### dashboard.html

Stats overview page. Four stat cards in a grid showing live counts from the database.

**Template variables:**

| Variable | Type | Description |
|----------|------|-------------|
| `user_count` | int | Total registered users |
| `room_count` | int | Total rooms |
| `message_count` | int | Non-deleted messages (DM messages excluded) |
| `banned_count` | int | Banned users |
| `deleted_count` | int | Soft-deleted messages (DM messages excluded) |
| `dm_count` | int | Number of DM channels |
| `new_users_24h` | int | Users registered in the last 24 hours |

The dashboard also fetches a live **Online Now** count from the internal bridge (`/api/online`) and shows chat-server / admin-portal status dots.

---

### monitor.html

Live chat monitor. Tabs for each room, auto-scrolling message feed, admin send bar, and emoji picker.

**Template variables:**

| Variable | Type | Description |
|----------|------|-------------|
| `rooms` | list of tuples | `(id, name, type)` for each room |
| `emoji_names` | list of str | Sorted emoji filename stems from `/opt/scenechat/emoji/` |

**JavaScript functions:**

| Function | Description |
|----------|-------------|
| `switchRoom(roomId, btn)` | Switch active room tab, reset feed and poll |
| `startPolling()` | Start 2-second interval poll |
| `pollMessages()` | Fetch `/api/messages/<room>?since=<id>`, append new messages |
| `appendMessage(msg)` | Render a message row into the feed |
| `renderContent(text)` | Replace `:token:` with `<img>` emoji tags |
| `escapeHtml(text)` | XSS-safe text rendering |
| `deleteMessage(msgId)` | Soft-delete via `/messages/delete/<id>` |
| `sendAdminMessage()` | POST to `/api/send_message` with current room and input text |
| `toggleEmojiPicker()` | Show/hide emoji picker panel |
| `insertEmoji(token)` | Append `:token:` to admin input field |

**Message row structure:**
```
[timestamp] [username] [content with inline emoji] [Del button]
```
Admin messages (`username === '[Admin]'`) render the username in green (`#39ff14`) instead of purple.

---

### users.html

User management table. Shows all users ordered by registration date descending.

**Template variables:**

| Variable | Type | Description |
|----------|------|-------------|
| `users` | list of tuples | `(id, username, created_at, is_banned, token_expiry, role)` |

**Per-row actions:**
- Ban / Unban button (moderator+)
- Role selector + Set button (admin/superadmin only, shown conditionally via `session.role`)
- Delete button (admin/superadmin only)

Role badges: Admin = green, Moderator = purple, User = blue.
Status badges: Banned = red, Active = green.

---

### logs.html

Server log viewer. Fetches from `/api/logs?lines=N` (JSON). Client-side filtering by level (ALL / ERROR / WARNING / INFO / DEBUG), text search with match highlighting, stats bar showing error and warning counts, auto-scroll to latest entry, auto-refresh every 3 seconds.

---

### maintenance.html

DB maintenance page. Shows table row counts and soft-deleted message count. Purge tools: deleted messages, messages older than N days, inactive users older than N days. Backup section with create, download, delete. Restore section with .sql file upload. All destructive actions have confirmation dialogs.

---

### rooms.html

Room management for created (text/voice) rooms. Create form at top, table of existing rooms below. **DM rooms are never listed here.**

**Template variables:**

| Variable | Type | Description |
|----------|------|-------------|
| `rooms` | list of tuples | `(id, name, type, created_at, password_hash, access_level)` — DM rooms excluded |

**Create form fields:** `name` (text), `type` (select: text/voice)

**Per-row actions:** set access level, set/clear password, delete (all refuse DM rooms).

Room type badges: Voice = purple, Text = blue.

---

### dms.html

DM channel moderation — **metadata only, never message content.** Lists each DM channel with its two participants, message count, last activity, and created date. A privacy notice makes the metadata-only nature explicit.

**Template variables:**

| Variable | Type | Description |
|----------|------|-------------|
| `dm_rooms` | list of tuples | `(room_id, participants, msg_count, last_activity, created_at)` |

**Per-row action:** Remove channel (deletes the DM room, its messages, and participants) with confirmation.

---

### mailbox.html

Admin mailbox with three sections: **Inbox** (mail addressed to the logged-in admin, unread highlighted, mark-read per item and mark-all-read), **Compose** (From pre-filled, To by username, message body), and **All Mail** (audit view of every message).

**Template variables:**

| Variable | Type | Description |
|----------|------|-------------|
| `inbox` | list of tuples | `(id, sender, content, sent_at, is_read)` for the logged-in admin |
| `mails` | list of tuples | `(id, sender, recipient, content, sent_at, is_read, is_deleted)` audit |

---

### update.html

OTA update hosting. Shows the current published version string and lets an admin upload a new `scenechat.xba` and set the version. Files are written to `/opt/scenechat/update/` and served (without auth) so Xbox clients can fetch them before login.

---

## OTA Update Hosting

The Xbox client supports over-the-air updates. The server side is simply a folder of static files served by the admin panel.

```
/opt/scenechat/update/
├── scenechat.ver       # plain-text version string, e.g. "1.3.0"
└── scenechat.xba       # packaged client release
```

- The admin **Update** page (`/update_mgmt`) shows the current version and lets an admin upload a new XBA and set the version string.
- The files are served at `/update/scenechat.ver` and `/update/scenechat.xba` **without authentication**, because the Xbox fetches them before any user logs in. Only these two static files are exposed; the folder contains nothing sensitive.
- On login the chat server compares the client's reported version against `scenechat.ver`. If newer, the client is prompted to download and apply the update, then relaunches.

Create the folder before first use:

```bash
sudo mkdir -p /opt/scenechat/update
sudo chown scenechat:scenechat /opt/scenechat/update
```

---

## Ports and Services

| Port | Protocol | Bind | Service | Auth |
|------|----------|------|---------|------|
| 8943 | TCP | 0.0.0.0 | SCCP chat server | DH + ChaCha20-Poly1305 |
| 7800 | UDP | 0.0.0.0 | Voice relay server | user_id + token_expiry DB check |
| 8950 | HTTP | 0.0.0.0 | Flask admin panel | bcrypt session |
| 8951 | HTTP | 127.0.0.1 | Admin API bridge | localhost only |

---

## Dependencies

### Python packages

```bash
pip3 install asyncio \
             mysql-connector-python \
             bcrypt \
             cryptography \
             flask \
             requests \
             pillow \
             --user
```

| Package | Used by | Purpose |
|---------|---------|---------|
| `asyncio` | server.py, voice_server.py | Async TCP/UDP event loop |
| `mysql-connector-python` | all | MySQL database access |
| `bcrypt` | server.py, admin.py | Password hashing and verification |
| `cryptography` | server.py | DH parameter generation |
| `flask` | admin.py | Web admin panel |
| `requests` | admin.py | HTTP POST to internal admin bridge |
| `logging.handlers` | server.py | RotatingFileHandler for log rotation |
| `pillow` | gen_emoji.py | Emoji PNG processing for atlas generation |

### System requirements

- Python 3.10+
- MySQL 5.7+ or MariaDB 10.3+
- Linux (tested on Ubuntu/Debian VPS)
- Ports 8943, 7800, 8950 open in firewall
- Port 8951 internal only (do not expose)

### Recommended systemd services

Run each process as a separate systemd service for automatic restart and logging:

```ini
# /etc/systemd/system/scenechat.service
[Unit]
Description=SceneChat Chat Server
After=network.target mysql.service

[Service]
ExecStart=/usr/bin/python3 /opt/scenechat/server.py
WorkingDirectory=/opt/scenechat
Restart=always
User=scenechat

[Install]
WantedBy=multi-user.target
```

Repeat for `scenechat-voice.service` (voice_server.py) and `scenechat-admin.service` (admin.py).