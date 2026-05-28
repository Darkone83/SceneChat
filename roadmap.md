# SceneChat Roadmap

**Team Resurgent / Darkone83**

This document tracks planned features and future development direction for SceneChat. Items are grouped by area and ordered roughly by dependency — features that unlock other features come first.

---

## Table of Contents

1. [Xbox Client](#xbox-client)
2. [Windows Client](#windows-client)
3. [Chat Features](#chat-features)
4. [Access Control](#access-control)
5. [Messaging](#messaging)
6. [Server and Infrastructure](#server-and-infrastructure)
7. [Admin Panel](#admin-panel)

---

## Xbox Client

### App Icon and Title ID
Assign a proper Xbox Title ID and XBE icon to SceneChat so it appears correctly on the dashboard and in launchers (XBMC, Avalaunch, UnleashX etc.) rather than as an unnamed homebrew XBE.

- Register a homebrew Title ID in the community-maintained range
- Create a 128x128 icon and a 64x32 title image in the Xbox XBE format
- Embed both in the XBE header using standard RXDK tooling
- Dashboard description string: "SceneChat — Private encrypted Xbox chat"
- Affects: XBE header, asset files, build configuration

---

### User List Panel
Display a live list of users currently connected to the server. Required before DMs can be initiated from the Xbox client — you need to know who is online to open a conversation.

**Client side:**
- New sidebar panel replacing or extending the room list
- Toggle between room list and user list via controller (e.g. L/R bumper or Y button)
- User entries show username and current room
- Selecting a user opens a context menu: View Profile / Send DM

**Protocol:**
- New packet type `SCCP_USER_LIST` (0x11) — server sends full online user list on login
- New packet type `SCCP_USER_JOIN` (0x12) — broadcast when a user connects
- New packet type `SCCP_USER_LEAVE` (0x13) — broadcast when a user disconnects
- Server maintains an online presence dict and pushes delta updates

**Server side:**
- `connected_clients` already tracks connected users — needs to broadcast presence events on connect/disconnect
- Admin panel monitor gains an online users sidebar showing live count

**DB changes:** none required for basic presence

---

## Windows Client

A native Windows desktop client that connects to the same SceneChat server using the identical SCCP protocol. Allows PC users to participate alongside Xbox users in the same rooms.

**Scope:**
- C++ Win32 or C# WPF application
- Identical DH + ChaCha20-Poly1305 handshake (share crypto code or re-implement)
- Full room list, message history, real-time receive
- USB keyboard as primary input (no OSK needed)
- Emoji rendering inline with text (GDI+ or Direct2D)
- System tray support with unread message notifications
- Auto-login via saved credentials (same `creds.dat` format or Windows credential store)

**Shared infrastructure:**
- Same server, same database, same protocol — no server changes required for basic text chat
- User list presence will show Windows users alongside Xbox users
- Admin panel sees Windows client messages identically to Xbox messages

---

## Chat Features

### Online Presence Carried to Client
Currently the server knows which users are connected (`connected_clients`) but the Xbox client has no visibility into who else is online. Presence should flow to both the Xbox client and the Windows client in real time.

**Implementation:**
- On login: server sends `SCCP_USER_LIST` with all currently connected users
- On any user connect: server broadcasts `SCCP_USER_JOIN` to all connected clients
- On any user disconnect: server broadcasts `SCCP_USER_LEAVE` to all connected clients
- Client maintains an in-memory online user table, refreshed by these events
- Admin panel `/api/online_users` endpoint queries the internal bridge (port 8951) for live data rather than the database

**DB changes:** optional `last_seen` column on `users` for persistent last-online timestamp

---

## Access Control

### Password Protected Rooms
Rooms that require a password to join. Useful for private group conversations without needing moderator-only access control.

**Client side:**
- Locked rooms display a 🔒 indicator in the room sidebar
- Selecting a locked room opens a password prompt using the existing OSK
- Wrong password shows an error and returns to room list

**Protocol:**
- `SCCP_JOIN_ROOM` payload extended: `[room_id 1B][password_len 1B][password]`
- Server checks hash on join, responds with `SCCP_AUTH_FAIL` + reason on wrong password

**Server side:**
- Room join handler checks `password_hash` column; if set, verifies submitted password with bcrypt

**Admin panel:**
- Lock/unlock toggle on the Rooms page
- Password set via admin form (hashed before storage, never shown again)

**DB changes:**
```sql
ALTER TABLE rooms ADD COLUMN password_hash VARCHAR(128) DEFAULT NULL;
```

---

### Access Control Rooms (Mod/Admin Only)
Rooms that are invisible and unjoinable by regular users. Useful for staff coordination channels, moderation discussion, and admin-only announcements.

**Room access levels:**
- `public` — visible and joinable by all authenticated users (current default)
- `moderator` — visible and joinable only by users with role `moderator`, `admin`, or `superadmin`
- `admin` — visible and joinable only by users with role `admin` or `superadmin`

**Client side:**
- Room list filtered server-side before sending — clients only receive rooms they can access
- No client-side changes needed beyond displaying whatever the server sends

**Protocol:**
- `SCCP_ROOM_LIST` response filtered by user role before sending
- `SCCP_JOIN_ROOM` rejected with `SCCP_ERROR` if user lacks permission

**Server side:**
- Role check on `handle_join_room()` against requesting user's role
- `SCCP_ROOM_LIST` query filters by `access_level` vs user role

**Admin panel:**
- Access level selector on room create form and room edit
- Access-controlled rooms visually distinguished in the Rooms table

**DB changes:**
```sql
ALTER TABLE rooms ADD COLUMN access_level ENUM('public', 'moderator', 'admin')
    NOT NULL DEFAULT 'public';
```

---

## Messaging

### Direct Messages (DMs)
Private one-to-one conversations between two users. Requires the online user list to be implemented first so users can initiate DMs from the Xbox client.

**Architecture:**
- DMs are implemented as special rooms with `type = 'dm'` and exactly two participants
- A `room_participants` join table tracks which users belong to each DM room
- The server finds or creates a DM room between two user IDs on demand

**Client side:**
- Select a user from the online user list → Send DM option
- DM rooms appear in a separate section of the room sidebar below public rooms
- DM room names display as the other user's username
- Unread indicator when a DM arrives while in another room

**Protocol:**
- New packet type `SCCP_DM_OPEN` (0x14) — client sends target user_id, server finds/creates DM room and responds with `SCCP_ROOM_INFO`
- `SCCP_MSG_RECV` used for delivery (same as room messages)
- DM rooms only broadcast to the two participants regardless of `room` field

**Server side:**
- `handle_dm_open()`: query `room_participants` for existing DM between two users, create if not found, respond with room info
- Broadcast logic checks participant list for DM rooms instead of `client['room']`

**DB changes:**
```sql
ALTER TABLE rooms ADD COLUMN type
    ENUM('text', 'voice', 'dm') NOT NULL DEFAULT 'text';

CREATE TABLE IF NOT EXISTS room_participants (
    room_id   INT NOT NULL,
    user_id   INT NOT NULL,
    joined_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (room_id, user_id),
    CONSTRAINT fk_rp_room FOREIGN KEY (room_id) REFERENCES rooms (id),
    CONSTRAINT fk_rp_user FOREIGN KEY (user_id) REFERENCES users (id)
);
```

---

### Mailbox System
Offline messaging — messages delivered to a user's inbox when they are not connected. Think of it as a persistent DM that waits for the recipient to come online.

**Use cases:**
- Leaving a message for a friend who is offline
- Admin announcements that need to reach users on next login
- System notifications (ban reason, role change, etc.)

**Client side:**
- Mailbox indicator on the main screen (unread count badge)
- Mailbox screen accessible from main menu or room list
- Messages listed with sender, timestamp, and content
- Mark as read / delete per message
- Compose new mail by selecting a user from a list

**Protocol:**
- New packet type `SCCP_MAIL_LIST` (0x15) — server sends unread mail count + list on login
- New packet type `SCCP_MAIL_SEND` (0x16) — client sends a mail to a user_id
- New packet type `SCCP_MAIL_READ` (0x17) — client marks a mail as read
- New packet type `SCCP_MAIL_DELETE` (0x18) — client deletes a mail

**Server side:**
- On login: query unread mail count, send `SCCP_MAIL_LIST` after `AUTH_OK`
- On `SCCP_MAIL_SEND`: validate recipient exists and is not banned, insert to `mailbox` table
- If recipient is currently online: also deliver via `SCCP_MAIL_LIST` push

**Admin panel:**
- Admin can send mail to any user from the Users page
- Mailbox stats on dashboard (total unread system-wide)

**DB changes:**
```sql
CREATE TABLE IF NOT EXISTS mailbox (
    id           INT      NOT NULL AUTO_INCREMENT,
    sender_id    INT      NOT NULL,
    recipient_id INT      NOT NULL,
    subject      VARCHAR(128)      DEFAULT '',
    content      TEXT     NOT NULL,
    sent_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    is_read      TINYINT(1) NOT NULL DEFAULT 0,
    is_deleted   TINYINT(1) NOT NULL DEFAULT 0,

    PRIMARY KEY (id),
    KEY idx_recipient (recipient_id, is_read),
    CONSTRAINT fk_mail_sender    FOREIGN KEY (sender_id)    REFERENCES users (id),
    CONSTRAINT fk_mail_recipient FOREIGN KEY (recipient_id) REFERENCES users (id)
);
```

---

## Server and Infrastructure

### Online Presence Persistence
The server currently loses all online state on restart. Presence should persist where meaningful.

**Planned:**
- `last_seen` column on `users` updated on disconnect
- `last_room` column on `users` — last room the user was in, restored on next login as default join target
- Admin panel shows last seen timestamp on Users page
- Xbox client shows "last seen X minutes ago" for offline users in the user list

**DB changes:**
```sql
ALTER TABLE users ADD COLUMN last_seen  DATETIME DEFAULT NULL;
ALTER TABLE users ADD COLUMN last_room  INT      DEFAULT NULL;
```

---

## Deleted Message Sync to Client

Currently when an admin soft-deletes a message from the monitor, the change only affects the database. Connected Xbox clients continue to display the deleted message until they reconnect and receive fresh history. Deletions should propagate to all connected clients in real time.

**Client side:**
- New packet type `SCCP_MSG_DELETE` (0x19) received from server
- Payload: `[room_id 1B][message_id 4B big-endian]`
- Client locates the message in its rendered message buffer by ID and replaces the content with `[deleted]` in muted colour
- Message IDs must be tracked per rendered message entry — currently messages are rendered without storing their DB ID on the client

**Protocol:**
- `SCCP_MSG_DELETE` (0x19) — server broadcasts to all clients in the affected room
- Sent immediately after `UPDATE messages SET is_deleted = 1` succeeds in the database
- Clients not currently in the affected room ignore the packet silently on receipt

**Server side:**
- `delete_message()` route in admin.py gains a second step after the DB update: POST to `http://127.0.0.1:8951/admin/delete`
- New `_handle_admin_http` route `POST /admin/delete` calls `admin_broadcast_delete(room_id, message_id)`
- `admin_broadcast_delete()` mirrors `admin_broadcast()` but sends `SCCP_MSG_DELETE` instead of `SCCP_MSG_RECV`
- Room ID for the deleted message is resolved by querying `messages.room_id` before broadcasting

**Admin panel:**
- No UI changes required — the existing Del button already calls `/messages/delete/<id>`
- The backend gains the broadcast step transparently

**DB changes:** none — `is_deleted` column already exists on the `messages` table

---

## Admin Panel

### Online Users Panel
Live sidebar or dedicated page showing all currently connected clients — username, current room, connection time. Queries the internal admin bridge (port 8951) rather than the database since DB has no live state.

### Mail Management
View and send mailbox messages from the admin panel. Compose mail to any user. View all system mail. Useful for announcements and moderation notices.

### Access Control Room Management
Room create/edit form gains an access level selector. Access-controlled rooms highlighted differently in the Rooms table.

---

## Schema Changelog

*(See DB.md for full schema history)*

| Planned version | Feature | SQL change |
|-----------------|---------|------------|
| 1.1 | Online persistence | `ALTER TABLE users ADD COLUMN last_seen DATETIME` |
| 1.1 | Online persistence | `ALTER TABLE users ADD COLUMN last_room INT` |
| 1.2 | Password rooms | `ALTER TABLE rooms ADD COLUMN password_hash VARCHAR(128)` |
| 1.2 | Access control rooms | `ALTER TABLE rooms ADD COLUMN access_level ENUM(...)` |
| 1.3 | DMs | `ALTER TABLE rooms ADD COLUMN type ENUM(..., 'dm')` |
| 1.3 | DMs | `CREATE TABLE room_participants (...)` |
| 1.4 | Mailbox | `CREATE TABLE mailbox (...)` |