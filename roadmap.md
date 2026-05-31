# SceneChat Roadmap

**Team Resurgent / Darkone83**

---

## Released

### v1.3 — DMs, Mailbox, OTA Updates *(released 2026-05-31)*

- **Direct Messages** — private one-to-one conversations via `SCCP_DM_OPEN`. Real-time delivery to both participants regardless of active room. DM rooms are excluded from the public room list and walled off from the admin panel (metadata-only DM management; content never exposed).
- **Mailbox** — offline messaging delivered to the inbox on login. `SCCP_MAIL_LIST / MAIL_SEND / MAIL_READ / MAIL_DELETE`. Supports sending to offline users by username.
- **OTA Update System** — Xbox client checks server version on login and can download/extract a new XBA package over HTTP, then relaunches. Admin panel hosts the version string and XBA package.
- **Admin additions** — mailbox management (inbox, mark-read, compose, audit), DM channel management (metadata only), update hosting page.
- **Clients** — DMs and mailbox shipped on Xbox (RXDK), Python (PySide6), and WPF clients.
- **Xbox App Icon and Title ID** — homebrew Title ID, 128×128 icon, and 64×32 title image assigned. Dashboard description: "SceneChat — Private encrypted Xbox chat".

---

## v1.4 — Video Chat *(next release)*

### Overview

Video chat support for the original Xbox. One dedicated video room. Xbox clients stream and receive video. Python and WPF clients can join the video room for text chat and passive video viewing if a stream is active — they cannot initiate a video stream.

---

### Architecture

```
Xbox (streamer)
    │
    │  UDP video frames → video_server.py
    │
video_server.py (relay)
    │
    ├─► Xbox clients (receive + display video + text)
    ├─► Python clients (text only + "Video streaming is Xbox only" notice)
    └─► WPF clients   (text only + "Video streaming is Xbox only" notice)
```

`video_server.py` is a separate asyncio UDP server, similar to `voice_server.py`. It relays encoded video frames from the streaming Xbox to all connected video room participants.

---

### DB Changes

```sql
ALTER TABLE rooms MODIFY COLUMN type
    ENUM('text','voice','dm','video') NOT NULL DEFAULT 'text';
```

One video room created via admin panel. Only one video room is supported in v1.4.

---

### Protocol Additions

```
SCCP_VIDEO_INIT   0x20  C→S  Xbox announces video stream start
SCCP_VIDEO_STOP   0x21  C→S  Xbox announces video stream end
SCCP_VIDEO_ACTIVE 0x22  S→C  Server notifies room a stream is active
SCCP_VIDEO_ENDED  0x23  S→C  Server notifies room stream has ended
```

Video frames are relayed over UDP directly via `video_server.py` — not through the TCP SCCP channel.

---

### video_server.py

- asyncio UDP server on a dedicated port (TBD — likely 7801)
- Accepts encoded video frames from one active Xbox streamer
- Relays frames to all connected video room participants
- Tracks active streamer — only one stream at a time
- Sends `SCCP_VIDEO_ACTIVE` / `SCCP_VIDEO_ENDED` to connected clients via internal bridge when stream state changes

---

### Xbox Client

- Video room type rendered in sidebar alongside voice rooms
- Joining video room: connects to `video_server.py` UDP port
- Can initiate stream (camera required) or join as viewer
- Video frame encode/decode — format TBD (likely raw YUV or simple MJPEG subset targeting Xbox hardware constraints)
- Text input and chat history work normally alongside video
- If no stream is active: room shows text chat, "No stream active" message

---

### Python Client

- Video room appears in room list as `[video]`
- Joining shows text chat area normally
- Persistent notice: `"Video streaming is Xbox only. Text chat available."`
- If a stream is active: display video frame area if feasible, otherwise notice remains
- Text entry and message history work as normal

---

### WPF Client

- Same behaviour as Python client
- Video room in room list as `[video]`
- Persistent notice in video area: `"Video streaming is Xbox only. Text chat available."`
- Text entry and message history work as normal

---

### Admin Panel

- Video room visible on Rooms page
- Online Users page shows who is in the video room
- Stream active/inactive status shown on dashboard

---

### Build Order

1. DB migration (`video` type on rooms)
2. `video_server.py`
3. Protocol additions (SCCP_VIDEO_*)
4. Xbox client — stream + view + UDP video
5. Python client — text + notice
6. WPF client — text + notice
7. Admin panel — stream status

---

## Schema Changelog

| Version | Date | Feature | SQL |
|---------|------|---------|-----|
| 1.0 | 2026-05-27 | Initial schema | `users`, `rooms`, `messages` |
| 1.1 | 2026-05-28 | Online persistence | `last_seen`, `last_room` on `users` |
| 1.2 | 2026-05-29 | Password rooms | `password_hash` on `rooms` |
| 1.2 | 2026-05-29 | Access control | `access_level` on `rooms` |
| 1.3 | 2026-05-31 | DMs | `type` extended on `rooms`, `room_participants` table |
| 1.3 | 2026-05-31 | Mailbox | `mailbox` table |
| 1.4 | Planned | Video Chat | `video` type on `rooms` |