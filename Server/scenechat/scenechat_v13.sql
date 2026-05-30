-- ============================================================================
-- SceneChat v1.3 — Baseline Database Setup
-- Team Resurgent / Darkone83
--
-- Usage (fresh install):
--   mysql -u root -p < scenechat_v13.sql
--
-- Usage (existing install — use migrations in db.md instead):
--   Apply only the ALTER/CREATE statements relevant to your current version.
--
-- ⚠️  Replace 'your_password_here' with a strong unique password before deploying.
--     Update DB_CONFIG in server.py and admin.py to match.
-- ============================================================================

-- ----------------------------------------------------------------------------
-- Database and user
-- ----------------------------------------------------------------------------

CREATE DATABASE IF NOT EXISTS scenechat
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

CREATE USER IF NOT EXISTS 'scenechat'@'localhost'
    IDENTIFIED BY 'your_password_here';

GRANT ALL PRIVILEGES ON scenechat.* TO 'scenechat'@'localhost';

FLUSH PRIVILEGES;

USE scenechat;

-- ----------------------------------------------------------------------------
-- users
-- Registered user accounts. Admin system user is created automatically.
-- ----------------------------------------------------------------------------

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

-- ----------------------------------------------------------------------------
-- rooms
-- Text, voice, and DM rooms. DM rooms are created server-side on SCCP_DM_OPEN.
-- ----------------------------------------------------------------------------

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

-- ----------------------------------------------------------------------------
-- messages
-- All chat messages. Soft-deleted only — hard purge via admin Maintenance page.
-- ----------------------------------------------------------------------------

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

-- ----------------------------------------------------------------------------
-- room_participants
-- Tracks the two participants in each DM room.
-- Each DM room has exactly two rows in this table.
-- ----------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS room_participants (
    room_id   INT      NOT NULL,
    user_id   INT      NOT NULL,
    joined_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (room_id, user_id),
    CONSTRAINT fk_rp_room FOREIGN KEY (room_id) REFERENCES rooms (id),
    CONSTRAINT fk_rp_user FOREIGN KEY (user_id) REFERENCES users (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ----------------------------------------------------------------------------
-- mailbox
-- Offline messages. Delivered to recipient inbox on next login.
-- Soft-deleted only.
-- ----------------------------------------------------------------------------

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

-- ----------------------------------------------------------------------------
-- Default rooms (optional — can also be created from admin panel)
-- ----------------------------------------------------------------------------

INSERT IGNORE INTO rooms (name, type) VALUES ('general',   'text');
INSERT IGNORE INTO rooms (name, type) VALUES ('off-topic', 'text');
INSERT IGNORE INTO rooms (name, type) VALUES ('hardware',  'text');
INSERT IGNORE INTO rooms (name, type) VALUES ('softmod',   'text');
INSERT IGNORE INTO rooms (name, type) VALUES ('dev',       'text');
INSERT IGNORE INTO rooms (name, type) VALUES ('showcase',  'text');
INSERT IGNORE INTO rooms (name, type) VALUES ('voice1',    'voice');
INSERT IGNORE INTO rooms (name, type) VALUES ('voice2',    'voice');

-- ============================================================================
-- Setup complete. Verify with:
--   mysql -u scenechat -p'your_password_here' scenechat -e "SHOW TABLES;"
-- ============================================================================
