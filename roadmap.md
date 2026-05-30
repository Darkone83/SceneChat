# SceneChat Roadmap

**Team Resurgent / Darkone83**

---

## v1.3 — Next Release

### Direct Messages
Private one-to-one conversations. Requires v1.2 user list as prerequisite.

**Protocol:** `SCCP_DM_OPEN 0x14`

**DB changes:**
```sql
ALTER TABLE rooms ADD COLUMN type ENUM('text','voice','dm') NOT NULL DEFAULT 'text';

CREATE TABLE IF NOT EXISTS room_participants (
    room_id   INT NOT NULL,
    user_id   INT NOT NULL,
    joined_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (room_id, user_id)
);
```

**Implementation:**
- Server: create or find existing DM room between two users on `SCCP_DM_OPEN`
- All clients: select user from online panel → open DM
- Xbox: navigate online overlay with D-pad, A to open DM
- Python/WPF: click user in online panel to open DM

---

### Mailbox System
Offline messaging — delivered to inbox on login when sender is offline.

**Protocol:**
```
SCCP_MAIL_LIST   0x15  unread mail on login
SCCP_MAIL_SEND   0x16  send mail to user_id
SCCP_MAIL_READ   0x17  mark as read
SCCP_MAIL_DELETE 0x18  delete mail
```

**DB changes:**
```sql
CREATE TABLE IF NOT EXISTS mailbox (
    id           INT NOT NULL AUTO_INCREMENT,
    sender_id    INT NOT NULL,
    recipient_id INT NOT NULL,
    content      TEXT NOT NULL,
    sent_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    is_read      TINYINT(1) NOT NULL DEFAULT 0,
    is_deleted   TINYINT(1) NOT NULL DEFAULT 0,
    PRIMARY KEY (id)
);
```

---

### Admin Panel Additions
- Mail management page — view and send mailbox messages
- DM room visibility and management

---

### Xbox App Icon and Title ID
Assign a proper homebrew Title ID, 128x128 icon and 64x32 title image.
Dashboard description: "SceneChat — Private encrypted Xbox chat"

---

## Schema Changelog

| Version | Date | Feature | SQL |
|---------|------|---------|-----|
| 1.0 | 2026-05-27 | Initial schema | `users`, `rooms`, `messages` |
| 1.1 | 2026-05-28 | Online persistence | `last_seen`, `last_room` on `users` |
| 1.2 | 2026-05-29 | Password rooms | `password_hash` on `rooms` |
| 1.2 | 2026-05-29 | Access control | `access_level` on `rooms` |
| 1.3 | Planned | DMs | `type` on `rooms`, `room_participants` table |
| 1.3 | Planned | Mailbox | `mailbox` table |

---

## v1.4 — Video Chat

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
-- Add video room type
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