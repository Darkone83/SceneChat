# SceneChat Database Setup

**Team Resurgent / Darkone83**

This document covers creating the SceneChat MySQL database from scratch. It will be updated as new features (DMs, password-protected rooms, etc.) are added.

---

## Table of Contents

1. [Requirements](#requirements)
2. [Create Database and User](#create-database-and-user)
3. [Create Tables](#create-tables)
4. [Verify Setup](#verify-setup)
5. [Default Data](#default-data)
6. [Backup and Restore](#backup-and-restore)
7. [Schema Changelog](#schema-changelog)

---

## Requirements

- MySQL 5.7+ or MariaDB 10.3+
- Root or equivalent access to create databases and users

---

## Create Database and User

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
    IDENTIFIED BY 'XbSceneChat01!';

GRANT ALL PRIVILEGES ON scenechat.* TO 'scenechat'@'localhost';

FLUSH PRIVILEGES;
```

---

## Create Tables

Switch to the database and create all tables:

```sql
USE scenechat;
```

### users

Stores registered user accounts. The `Admin` system user (used for admin panel broadcasts) is inserted automatically by the server when needed.

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
- `password_hash` — bcrypt output (rounds=12), stored as a 60-character string
- `salt` — bcrypt salt, stored separately for reference; bcrypt hash already embeds the salt
- `role` — controls admin panel access; `user` is the default for all registered accounts
- `is_banned` — 1 = banned, login rejected at the server level before password check
- `token` — 128-character hex string (`secrets.token_hex(64)`), issued on successful login
- `token_expiry` — used by the voice server to validate active sessions
- `last_seen` — updated on clean disconnect; NULL if user has never disconnected cleanly
- `last_room` — room ID the user was in at last disconnect; used by the admin panel Users page

---

### rooms

Stores chat and voice rooms. Rooms are created from the admin panel (superadmin only).

```sql
CREATE TABLE IF NOT EXISTS rooms (
    id          INT         NOT NULL AUTO_INCREMENT,
    name        VARCHAR(64) NOT NULL,
    type        ENUM('text', 'voice') NOT NULL DEFAULT 'text',
    created_at  DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (id),
    UNIQUE KEY uq_room_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

**Column notes:**
- `type` — `text` rooms appear in the Xbox client chat sidebar; `voice` rooms are handled by voice_server.py over UDP

---

### messages

Stores all chat messages. Deletion is soft — `is_deleted = 1` hides the message in the client and admin monitor but preserves the record.

```sql
CREATE TABLE IF NOT EXISTS messages (
    id          INT      NOT NULL AUTO_INCREMENT,
    room_id     INT      NOT NULL,
    user_id     INT      NOT NULL,
    content     TEXT     NOT NULL,
    sent_at     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    is_deleted  TINYINT(1) NOT NULL DEFAULT 0,

    PRIMARY KEY (id),
    KEY idx_room_sent   (room_id, sent_at),
    KEY idx_room_id     (room_id, id),
    CONSTRAINT fk_msg_room FOREIGN KEY (room_id) REFERENCES rooms (id),
    CONSTRAINT fk_msg_user FOREIGN KEY (user_id) REFERENCES users (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

**Column notes:**
- `content` — plain UTF-8 text, may contain `:token:` emoji sequences
- `is_deleted` — soft delete only; records are never hard-deleted by normal operations
- `idx_room_sent` — used by history queries (ORDER BY sent_at DESC LIMIT 50)
- `idx_room_id` — used by admin monitor polling (WHERE room_id = ? AND id > ?)

---

## Schema Migrations

Apply these in order when upgrading from a previous version.

### v1.0 → v1.1

```sql
ALTER TABLE users ADD COLUMN last_seen DATETIME DEFAULT NULL;
ALTER TABLE users ADD COLUMN last_room INT DEFAULT NULL;
```

### v1.1 → v1.2 (planned)

```sql
ALTER TABLE rooms ADD COLUMN password_hash VARCHAR(128) DEFAULT NULL;
ALTER TABLE rooms ADD COLUMN access_level ENUM('public','moderator','admin')
    NOT NULL DEFAULT 'public';
```

---

## Verify Setup

Confirm all three tables exist with the correct structure:

```sql
USE scenechat;
SHOW TABLES;
```

Expected output:
```
+---------------------+
| Tables_in_scenechat |
+---------------------+
| messages            |
| rooms               |
| users               |
+---------------------+
```

Verify columns:

```sql
DESCRIBE users;
DESCRIBE rooms;
DESCRIBE messages;
```

Test the `scenechat` user can connect:

```bash
mysql -u scenechat -p'XbSceneChat01!' scenechat -e "SHOW TABLES;"
```

---

## Default Data

SceneChat has no required seed data — rooms and users are created through normal operation. However, you will need at least one room before any Xbox client can join.

Create your initial rooms from the admin panel after the first server start, or insert them directly:

```sql
USE scenechat;

INSERT INTO rooms (name, type) VALUES ('general', 'text');
INSERT INTO rooms (name, type) VALUES ('off-topic', 'text');
INSERT INTO rooms (name, type) VALUES ('voice', 'voice');
```

The `Admin` system user is created automatically by the server on the first admin broadcast — you do not need to insert it manually.

---

## Backup and Restore

### Via Admin Panel

The admin panel Maintenance page provides one-click backup and restore:
- Backups saved to `/opt/scenechat/backups/` on the server
- Download, delete, or restore from the panel
- Always back up before restoring — restore overwrites the current database

### Via Command Line

### Full backup

```bash
mysqldump -u scenechat -p'XbSceneChat01!' scenechat > scenechat_backup.sql
```

### Restore from backup

```bash
mysql -u scenechat -p'XbSceneChat01!' scenechat < scenechat_backup.sql
```

### Messages only (lighter backup)

```bash
mysqldump -u scenechat -p'XbSceneChat01!' scenechat messages > messages_backup.sql
```

---

## Schema Changelog

| Version | Date | Change |
|---------|------|--------|
| 1.0 | 2026-05-27 | Initial schema — `users`, `rooms`, `messages` |
| 1.1 | 2026-05-28 | Online persistence — `last_seen DATETIME`, `last_room INT` on `users` |
| 1.2 | Planned | Password rooms — `password_hash VARCHAR(128)` on `rooms` |
| 1.2 | Planned | Access control — `access_level ENUM` on `rooms` |
| 1.3 | Planned | DMs — `type ENUM` on `rooms`, `room_participants` table |
| 1.3 | Planned | Mailbox — `mailbox` table |

---

*See ROADMAP.md for the full v1.2 and v1.3 feature plan.*