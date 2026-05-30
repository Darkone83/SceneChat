# SceneChat Database Setup

**Team Resurgent / Darkone83**

This document covers creating the SceneChat MySQL/MariaDB database from scratch, applying schema migrations for existing installations, and managing backups. Current schema version: **v1.3**.

---

## Table of Contents

1. [Requirements](#requirements)
2. [Quick Start — SQL File](#quick-start--sql-file)
3. [Manual Setup](#manual-setup)
4. [Schema Migrations](#schema-migrations)
5. [Verify Setup](#verify-setup)
6. [Default Data](#default-data)
7. [Backup and Restore](#backup-and-restore)
8. [Schema Changelog](#schema-changelog)

---

## Requirements

- MySQL 5.7+ or MariaDB 10.3+
- Root or equivalent access to create databases and users
- `mysqldump` and `mysql` CLI tools (required for admin panel backup/restore)

---

## Quick Start — SQL File

The fastest way to set up a fresh installation is to use the included `scenechat_v13.sql` baseline file. This creates the database, user, and all tables in one step.

```bash
mysql -u root -p < scenechat_v13.sql
```

Then verify:

```bash
mysql -u scenechat -p'your_password_here' scenechat -e "SHOW TABLES;"
```

> ⚠️ Change `your_password_here` to a strong unique password before deploying. Update `DB_CONFIG` in both `server.py` and `admin.py` to match.

---

## Manual Setup

### 1. Create Database and User

Log in as root:

```bash
mysql -u root -p
```

Run the following:

```sql
CREATE DATABASE IF NOT EXISTS scenechat
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

CREATE USER IF NOT EXISTS 'scenechat'@'localhost'
    IDENTIFIED BY 'your_password_here';

GRANT ALL PRIVILEGES ON scenechat.* TO 'scenechat'@'localhost';

FLUSH PRIVILEGES;
```

### 2. Create Tables

Switch to the database:

```sql
USE scenechat;
```

---

### users

Stores registered user accounts.

```sql
CREATE TABLE IF NOT EXISTS users (
    id             INT          NOT NULL AUTO_INCREMENT,
    username       VARCHAR(32)  NOT NULL,
    password_hash  VARCHAR(128) NOT NULL,
    salt           VARCHAR(64)  NOT NULL DEFAULT '',
    role           ENUM('user', 'moderator', 'admin') NOT NULL DEFAULT 'user',
    is_banned      TINYINT(1)   NOT NULL DEFAULT 0,
    token          VARCHAR(128)          DEFAULT NULL,
    token_expiry   DATETIME              DEFAULT NULL,
    created_at     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_seen      DATETIME              DEFAULT NULL,
    last_room      INT                   DEFAULT NULL,

    PRIMARY KEY (id),
    UNIQUE KEY uq_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

**Column notes:**
- `password_hash` — bcrypt output (rounds=12)
- `salt` — stored separately for reference; bcrypt hash already embeds the salt
- `role` — controls admin panel access; `user` is the default for all registered accounts
- `is_banned` — 1 = banned; login rejected at the server level before password check
- `token` — 128-character hex string (`secrets.token_hex(64)`), issued on successful login
- `token_expiry` — used by the voice server to validate active sessions
- `last_seen` — updated on clean disconnect; NULL if never disconnected cleanly
- `last_room` — room ID at last disconnect; shown in admin panel Users page

---

### rooms

Stores text, voice, and DM rooms.

```sql
CREATE TABLE IF NOT EXISTS rooms (
    id            INT         NOT NULL AUTO_INCREMENT,
    name          VARCHAR(64) NOT NULL,
    type          ENUM('text', 'voice', 'dm') NOT NULL DEFAULT 'text',
    password_hash VARCHAR(128)          DEFAULT NULL,
    access_level  ENUM('public', 'moderator', 'admin') NOT NULL DEFAULT 'public',
    created_at    DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (id),
    UNIQUE KEY uq_room_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

**Column notes:**
- `type` — `text` = public chat room; `voice` = UDP voice room; `dm` = private DM between two users
- `password_hash` — bcrypt hash; NULL means no password required
- `access_level` — `public` = all users; `moderator` = mods and admins only; `admin` = admins only

---

### messages

Stores all chat messages. Deletion is soft — `is_deleted = 1` hides the message but preserves the record.

```sql
CREATE TABLE IF NOT EXISTS messages (
    id          INT        NOT NULL AUTO_INCREMENT,
    room_id     INT        NOT NULL,
    user_id     INT        NOT NULL,
    content     TEXT       NOT NULL,
    sent_at     DATETIME   NOT NULL DEFAULT CURRENT_TIMESTAMP,
    is_deleted  TINYINT(1) NOT NULL DEFAULT 0,

    PRIMARY KEY (id),
    KEY idx_room_sent (room_id, sent_at),
    KEY idx_room_id   (room_id, id),
    CONSTRAINT fk_msg_room FOREIGN KEY (room_id) REFERENCES rooms (id),
    CONSTRAINT fk_msg_user FOREIGN KEY (user_id) REFERENCES users (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

**Column notes:**
- `content` — plain UTF-8 text, may contain `:token:` emoji sequences
- `is_deleted` — soft delete; hard purge available from admin panel Maintenance page
- `idx_room_sent` — used by history queries
- `idx_room_id` — used by admin monitor polling

---

### room_participants

Tracks which users are participants in a DM room. Each DM room has exactly two entries.

```sql
CREATE TABLE IF NOT EXISTS room_participants (
    room_id   INT      NOT NULL,
    user_id   INT      NOT NULL,
    joined_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (room_id, user_id),
    CONSTRAINT fk_rp_room FOREIGN KEY (room_id) REFERENCES rooms (id),
    CONSTRAINT fk_rp_user FOREIGN KEY (user_id) REFERENCES users (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

**Column notes:**
- `room_id` — references a room with `type = 'dm'`
- `user_id` — one of the two participants
- Each DM room has exactly two rows in this table

---

### mailbox

Stores offline messages. Delivered to the recipient's inbox on next login if they are not currently connected.

```sql
CREATE TABLE IF NOT EXISTS mailbox (
    id           INT        NOT NULL AUTO_INCREMENT,
    sender_id    INT        NOT NULL,
    recipient_id INT        NOT NULL,
    content      TEXT       NOT NULL,
    sent_at      DATETIME   NOT NULL DEFAULT CURRENT_TIMESTAMP,
    is_read      TINYINT(1) NOT NULL DEFAULT 0,
    is_deleted   TINYINT(1) NOT NULL DEFAULT 0,

    PRIMARY KEY (id),
    KEY idx_mailbox_recipient (recipient_id, is_read),
    CONSTRAINT fk_mail_sender    FOREIGN KEY (sender_id)    REFERENCES users (id),
    CONSTRAINT fk_mail_recipient FOREIGN KEY (recipient_id) REFERENCES users (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

**Column notes:**
- `sender_id` — user who sent the mail
- `recipient_id` — intended recipient
- `is_read` — 0 = unread; updated when recipient views the message
- `is_deleted` — soft delete; mail is never hard-deleted by normal operations
- `idx_mailbox_recipient` — used to fetch unread mail on login

---

## Schema Migrations

Apply these in order when upgrading an existing installation. Always back up first.

### v1.0 → v1.1

```sql
ALTER TABLE users ADD COLUMN last_seen DATETIME DEFAULT NULL;
ALTER TABLE users ADD COLUMN last_room INT DEFAULT NULL;
```

### v1.1 → v1.2

```sql
ALTER TABLE rooms ADD COLUMN password_hash VARCHAR(128) DEFAULT NULL;
ALTER TABLE rooms ADD COLUMN access_level ENUM('public','moderator','admin')
    NOT NULL DEFAULT 'public';
```

### v1.2 → v1.3

```sql
-- Extend rooms type to support DMs
ALTER TABLE rooms MODIFY COLUMN type
    ENUM('text','voice','dm') NOT NULL DEFAULT 'text';

-- DM participant tracking
CREATE TABLE IF NOT EXISTS room_participants (
    room_id   INT      NOT NULL,
    user_id   INT      NOT NULL,
    joined_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (room_id, user_id),
    CONSTRAINT fk_rp_room FOREIGN KEY (room_id) REFERENCES rooms (id),
    CONSTRAINT fk_rp_user FOREIGN KEY (user_id) REFERENCES users (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Mailbox for offline messaging
CREATE TABLE IF NOT EXISTS mailbox (
    id           INT        NOT NULL AUTO_INCREMENT,
    sender_id    INT        NOT NULL,
    recipient_id INT        NOT NULL,
    content      TEXT       NOT NULL,
    sent_at      DATETIME   NOT NULL DEFAULT CURRENT_TIMESTAMP,
    is_read      TINYINT(1) NOT NULL DEFAULT 0,
    is_deleted   TINYINT(1) NOT NULL DEFAULT 0,
    PRIMARY KEY (id),
    KEY idx_mailbox_recipient (recipient_id, is_read),
    CONSTRAINT fk_mail_sender    FOREIGN KEY (sender_id)    REFERENCES users (id),
    CONSTRAINT fk_mail_recipient FOREIGN KEY (recipient_id) REFERENCES users (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

---

## Verify Setup

```sql
USE scenechat;
SHOW TABLES;
```

Expected output (v1.3):
```
+---------------------+
| Tables_in_scenechat |
+---------------------+
| mailbox             |
| messages            |
| room_participants   |
| rooms               |
| users               |
+---------------------+
```

Verify columns on key tables:

```sql
DESCRIBE rooms;
DESCRIBE mailbox;
DESCRIBE room_participants;
```

Test the `scenechat` user can connect:

```bash
mysql -u scenechat -p'your_password_here' scenechat -e "SHOW TABLES;"
```

---

## Default Data

No seed data is required. Rooms and users are created through normal operation. At least one room is needed before any client can join.

Create initial rooms from the admin panel, or insert directly:

```sql
USE scenechat;

INSERT INTO rooms (name, type) VALUES ('general', 'text');
INSERT INTO rooms (name, type) VALUES ('off-topic', 'text');
INSERT INTO rooms (name, type) VALUES ('voice1', 'voice');
```

The `Admin` system user is created automatically by the server on the first admin broadcast.

The `scene_bot` user must be registered through any client, then upgraded to Admin via the admin panel. Note its user ID after creation — it is referenced as `SCENE_BOT_ID` in `server.py`.

---

## Backup and Restore

### Via Admin Panel

The admin panel Maintenance page provides one-click backup and restore:
- Backups saved to `/opt/scenechat/backups/` on the server
- Download, delete, or restore from the panel
- Always back up before restoring — restore overwrites the current database
- Both the chat server and admin panel must be restarted after restore

### Via Command Line

Full backup:
```bash
mysqldump -u scenechat -p'your_password_here' scenechat > scenechat_backup.sql
```

Restore:
```bash
mysql -u scenechat -p'your_password_here' scenechat < scenechat_backup.sql
```

Messages only (lighter):
```bash
mysqldump -u scenechat -p'your_password_here' scenechat messages > messages_backup.sql
```

---

## Schema Changelog

| Version | Date | Change |
|---------|------|--------|
| 1.0 | 2026-05-27 | Initial schema — `users`, `rooms`, `messages` |
| 1.1 | 2026-05-28 | Online persistence — `last_seen`, `last_room` on `users` |
| 1.2 | 2026-05-29 | Password rooms — `password_hash` on `rooms` |
| 1.2 | 2026-05-29 | Access control — `access_level` on `rooms` |
| 1.3 | Planned | DMs — `type` extended on `rooms`, new `room_participants` table |
| 1.3 | Planned | Mailbox — new `mailbox` table |

---

*See ROADMAP.md for the full v1.3 feature plan.*