# SceneChat Roadmap

**Team Resurgent / Darkone83**

---

## Table of Contents

1. [v1.2 — Next Release](#v12--next-release)
2. [v1.3 — Future](#v13--future)
3. [Schema Changelog](#schema-changelog)

---

## v1.2 — Next Release

### Protocol Additions

```
SCCP_USER_LIST  0x11  [count 1B] per user: [user_id 4B][username str8][room_id 1B]
SCCP_USER_JOIN  0x12  [user_id 4B][username str8][room_id 1B]
SCCP_USER_LEAVE 0x13  [user_id 4B][username str8]
SCCP_JOIN_ROOM  0x08  extended: [room_id 1B][password_len 1B][password str]
                      password_len=0 means no password
SCCP_AUTH_FAIL  0x06  reused for wrong room password response
```

### DB Migrations

```sql
ALTER TABLE rooms ADD COLUMN password_hash VARCHAR(128) DEFAULT NULL;
ALTER TABLE rooms ADD COLUMN access_level ENUM('public','moderator','admin')
    NOT NULL DEFAULT 'public';
```

### Build Order

1. DB migrations
2. Server
3. Xbox client
4. Python client
5. WPF client
6. Admin panel

---

### Server Changes

- On login: send `SCCP_USER_LIST` of all currently connected users
- On connect: broadcast `SCCP_USER_JOIN` to all connected clients
- On disconnect: broadcast `SCCP_USER_LEAVE` to all connected clients
- `handle_join_room`: check `password_hash`, reject with `SCCP_AUTH_FAIL` if wrong
- `SCCP_ROOM_LIST`: filter by `access_level` based on caller role
- Room list response: include `password_flag` byte (1=has password, 0=open) and `access_level`

---

### Xbox Client

- `sc_net.h`: add `SCCP_USER_LIST/JOIN/LEAVE`, `SC_User` struct, `SC_Net_RecvUserList/Join/Leave`
- `sc_net.cpp`: implement the three recv functions
- `chat.cpp`: in-memory user table `s_users[SC_MAX_USERS]`, updated on JOIN/LEAVE
- R3 button toggles user overlay — draws over chat area showing online users + their room
- Password prompt: reuse existing HID keyboard / on-screen KB when joining a locked room
- `SC_Net_SendJoinRoom` extended to include password field (len=0 for open rooms)

---

### Python Client

- `sc_net.py`: parse `USER_LIST/JOIN/LEAVE`, emit `sig_user_list`, `sig_user_join`, `sig_user_leave`
- `sc_ui.py`: user list panel (sidebar or overlay), lock icon on password rooms
- Password prompt dialog on join attempt for locked rooms

---

### WPF Client

- `ScProtocol.cs`: add `USER_LIST/JOIN/LEAVE` constants
- `ChatClient.cs`: `OnUserList`, `OnUserJoin`, `OnUserLeave` events
- `MainWindow.xaml.cs`: user list panel/flyout, lock icon on room items
- Password prompt dialog

---

### Admin Panel

- Online Users page: live table — username, current room, connect time
- Room create/edit: add password field and access level selector

---

## v1.3 — Future

### Direct Messages
Private one-to-one conversations. Requires v1.2 user list first.

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

---

### Mailbox System
Offline messaging delivered on login.

**Protocol:** `SCCP_MAIL_LIST 0x15`, `SCCP_MAIL_SEND 0x16`, `SCCP_MAIL_READ 0x17`, `SCCP_MAIL_DELETE 0x18`

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

### Admin Panel Additions (v1.3)
- Mail management page
- Access control room management

---

### Xbox App Icon and Title ID
Assign a proper homebrew Title ID, 128x128 icon and 64x32 title image.
Dashboard description: "SceneChat — Private encrypted Xbox chat"

---

## Schema Changelog

| Version | Feature | SQL |
|---------|---------|-----|
| 1.0 | Initial schema | `users`, `rooms`, `messages` |
| 1.1 | Online persistence | `last_seen`, `last_room` on `users` |
| 1.2 | Password rooms | `password_hash` on `rooms` |
| 1.2 | Access control | `access_level` on `rooms` |
| 1.3 | DMs | `type` on `rooms`, `room_participants` table |
| 1.3 | Mailbox | `mailbox` table |