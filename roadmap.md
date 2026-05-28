# SceneChat Roadmap

**Team Resurgent / Darkone83**

This document tracks planned features and future development direction for SceneChat. Items are grouped by area and ordered roughly by dependency.

---

## Table of Contents

1. [Xbox Client](#xbox-client)
2. [Chat Features](#chat-features)
3. [Access Control](#access-control)
4. [Messaging](#messaging)
5. [Server and Infrastructure](#server-and-infrastructure)
6. [Admin Panel](#admin-panel)

---

## Xbox Client

### App Icon and Title ID
Assign a proper Xbox Title ID and XBE icon to SceneChat so it appears correctly on the dashboard and in launchers.

- Register a homebrew Title ID in the community-maintained range
- Create a 128x128 icon and a 64x32 title image in the Xbox XBE format
- Embed both in the XBE header using standard RXDK tooling
- Dashboard description string: "SceneChat — Private encrypted Xbox chat"

---

### User List Panel
Display a live list of users currently connected to the server. Required before DMs can be initiated from the Xbox client.

**Client side:**
- New sidebar panel replacing or extending the room list
- Toggle between room list and user list via controller (e.g. L/R bumper or Y button)
- User entries show username and current room
- Selecting a user opens a context menu: View Profile / Send DM

**Protocol:**
- `SCCP_USER_LIST` (0x11) — server sends full online user list on login
- `SCCP_USER_JOIN` (0x12) — broadcast when a user connects
- `SCCP_USER_LEAVE` (0x13) — broadcast when a user disconnects

**Server side:**
- `connected_clients` already tracks connected users — needs to broadcast presence events on connect/disconnect

**DB changes:** none

---

## Chat Features

### Online Presence Carried to Client
Server knows who is connected but clients have no visibility. Presence should flow in real time.

**Implementation:**
- On login: server sends `SCCP_USER_LIST` with all currently connected users
- On connect: server broadcasts `SCCP_USER_JOIN` to all connected clients
- On disconnect: server broadcasts `SCCP_USER_LEAVE` to all connected clients
- Client maintains an in-memory online user table

---

## Access Control

### Password Protected Rooms
Rooms that require a password to join.

**Protocol:**
- `SCCP_JOIN_ROOM` payload extended: `[room_id 1B][password_len 1B][password]`
- Server checks hash on join, responds with `SCCP_AUTH_FAIL` on wrong password

**DB changes:**
```sql
ALTER TABLE rooms ADD COLUMN password_hash VARCHAR(128) DEFAULT NULL;
```

---

### Access Control Rooms (Mod/Admin Only)
Rooms invisible and unjoinable by regular users.

**Room access levels:** `public`, `moderator`, `admin`

**Server side:**
- `SCCP_ROOM_LIST` filtered by user role before sending
- `SCCP_JOIN_ROOM` rejected if user lacks permission

**DB changes:**
```sql
ALTER TABLE rooms ADD COLUMN access_level ENUM('public','moderator','admin')
    NOT NULL DEFAULT 'public';
```

---

## Messaging

### Direct Messages (DMs)
Private one-to-one conversations. Requires user list panel first.

**Architecture:**
- DMs implemented as special rooms with `type = 'dm'` and exactly two participants
- `room_participants` join table tracks participants

**Protocol:**
- `SCCP_DM_OPEN` (0x14) — client sends target user_id, server finds/creates DM room

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

---

### Mailbox System
Offline messaging — messages delivered to a user's inbox when not connected.

**Protocol:**
- `SCCP_MAIL_LIST` (0x15) — unread mail on login
- `SCCP_MAIL_SEND` (0x16) — send mail
- `SCCP_MAIL_READ` (0x17) — mark as read
- `SCCP_MAIL_DELETE` (0x18) — delete mail

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

## Server and Infrastructure

### Deleted Message Sync to Client
When an admin deletes a message from the monitor, propagate the deletion to connected clients in real time.

**Protocol:**
- `SCCP_MSG_DELETE` (0x19) — `[room_id 1B][message_id 4B]` broadcast to room

**Server side:**
- Admin delete route POSTs to internal bridge after DB update
- Bridge broadcasts `SCCP_MSG_DELETE` to affected room

**DB changes:** none — `is_deleted` already exists

---

## Admin Panel

### Online Users Panel
Live page showing all connected clients — username, current room, connection time.

### Mail Management
View and send mailbox messages from the admin panel.

### Access Control Room Management
Room create/edit form gains access level selector.

---

## Schema Changelog

| Version | Feature | SQL |
|---------|---------|-----|
| 1.0 | Initial schema | `users`, `rooms`, `messages` |
| 1.1 | Online persistence | `last_seen`, `last_room` on `users` |
| 1.2 | Password rooms | `password_hash` on `rooms` |
| 1.2 | Access control | `access_level` on `rooms` |
| 1.3 | DMs | `type` on `rooms`, `room_participants` table |
| 1.4 | Mailbox | `mailbox` table |