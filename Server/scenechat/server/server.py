import asyncio
import struct
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs
import bcrypt
import secrets
import os
import logging
import mysql.connector
from datetime import datetime, timedelta
from cryptography.hazmat.primitives.asymmetric.dh import generate_parameters
from cryptography.hazmat.primitives import serialization, hashes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.backends import default_backend

# -- Logging -------------------------------------------------------------------
os.makedirs('/opt/scenechat/logs', exist_ok=True)
logging.basicConfig(
    filename='/opt/scenechat/logs/scenechat.log',
    level=logging.INFO,
    format='%(asctime)s %(levelname)s %(message)s'
)
log = logging.getLogger('scenechat')

# -- Packet types --------------------------------------------------------------
SCCP_DH_INIT     = 0x01
SCCP_DH_RESPONSE = 0x02
SCCP_REGISTER    = 0x03
SCCP_LOGIN       = 0x04
SCCP_AUTH_OK     = 0x05
SCCP_AUTH_FAIL   = 0x06
SCCP_ROOM_LIST   = 0x07
SCCP_JOIN_ROOM   = 0x08
SCCP_ROOM_INFO   = 0x09
SCCP_MESSAGE     = 0x0A
SCCP_MSG_RECV    = 0x0B
SCCP_HISTORY     = 0x0C
SCCP_ERROR       = 0x0D
SCCP_PING        = 0x0E
SCCP_PONG        = 0x0F
SCCP_DISCONNECT  = 0x10

# -- Database configuration ----------------------------------------------------
DB_CONFIG = {
    'host':     'localhost',
    'user':     'scenechat',
    'password': 'XbSceneChat01!',
    'database': 'scenechat'
}

# -- Generate DH parameters once at startup ------------------------------------
log.info("Generating DH parameters...")
print("[*] Generating DH parameters, please wait...")
DH_PARAMETERS = generate_parameters(generator=2, key_size=1024, backend=default_backend())
log.info("DH parameters ready")
print("[*] DH parameters ready")

# -- Admin message queue (thread-safe bridge from Flask to asyncio)
_admin_queue = asyncio.Queue()

# -- Connected clients ---------------------------------------------------------
# user_id -> { writer, username, session_key, room, lock }
connected_clients = {}
muted_users       = set()   # in-memory mute list, clears on restart

# -- scene_bot constants -------------------------------------------------------
SCENE_BOT_ID       = 12
SCENE_BOT_USERNAME = 'scene_bot'

EMOJI_LIST = [
    'smile','wink','laugh','cry','angry','sad','surprised','thinking',
    'cool','love_face','dead','party','question','thumbs_up','thumbs_down',
    'check','x','alert','heart','fireheart','star','skull','fire',
    'scenechat_sc','scenechat_softmod','scenechat_fire','scenechat_modchip',
    'scenechat_controller_chat','scenechat_ping','scenechat_lobby',
    'scenechat_dpad','scenechat_buttons','scenechat_devbuild',
]

# -- Database ------------------------------------------------------------------
def get_db():
    return mysql.connector.connect(**DB_CONFIG)

# -- Crypto --------------------------------------------------------------------
def derive_key(shared_secret):
    # Strip leading zeros to match Xbox bignum output
    stripped = shared_secret.lstrip(b'\x00') or b'\x00'
    log.debug(f"Shared secret length after strip: {len(stripped)}")
    hkdf = HKDF(
        algorithm=hashes.SHA256(),
        length=32,
        salt=None,
        info=b'scenechat-session',
        backend=default_backend()
    )
    key = hkdf.derive(stripped)
    log.debug(f"Session key first 8 bytes: {key[:8].hex()}")
    return key

# -- Packet I/O ----------------------------------------------------------------
# Packet format: [2 bytes total len BE][1 byte type][payload]
# Encrypted:     [2 bytes total len BE][1 byte type][12 byte nonce][ChaCha20-Poly1305 ciphertext+tag]

async def read_packet(reader, timeout=300.0):
    try:
        header = await asyncio.wait_for(reader.readexactly(2), timeout=timeout)
        total_len = struct.unpack('>H', header)[0]
        log.debug(f"read_packet: total_len={total_len}")
        if total_len < 3:
            log.warning(f"read_packet: total_len too small: {total_len}")
            return None, None
        rest = await asyncio.wait_for(reader.readexactly(total_len - 2), timeout=30.0)
        log.debug(f"read_packet: type=0x{rest[0]:02X} payload_len={len(rest)-1}")
        return rest[0], bytes(rest[1:])
    except asyncio.TimeoutError:
        log.warning("read_packet: timeout")
        return None, None
    except Exception as e:
        log.error(f"read_packet error: {e}")
        return None, None

async def write_packet(writer, lock, pkt_type, payload=b''):
    data = bytes([pkt_type]) + payload
    total_len = len(data) + 2
    log.debug(f"write_packet: type=0x{pkt_type:02X} total_len={total_len}")
    async with lock:
        writer.write(struct.pack('>H', total_len) + data)
        await writer.drain()

async def write_encrypted(writer, lock, session_key, pkt_type, payload=b''):
    chacha = ChaCha20Poly1305(session_key)
    nonce  = os.urandom(12)
    encrypted = chacha.encrypt(nonce, payload, None)
    log.debug(f"write_encrypted: type=0x{pkt_type:02X} plain_len={len(payload)} enc_len={len(encrypted)}")
    await write_packet(writer, lock, pkt_type, nonce + encrypted)

async def read_encrypted(reader, session_key, timeout=300.0):
    pkt_type, payload = await read_packet(reader, timeout=timeout)
    if pkt_type is None or len(payload) < 12:
        log.warning(f"read_encrypted: bad packet type={pkt_type} payload_len={len(payload) if payload else 0}")
        return None, None
    try:
        chacha    = ChaCha20Poly1305(session_key)
        nonce     = payload[:12]
        decrypted = chacha.decrypt(nonce, payload[12:], None)
        log.debug(f"read_encrypted: type=0x{pkt_type:02X} decrypted_len={len(decrypted)}")
        return pkt_type, decrypted
    except Exception as e:
        log.error(f"read_encrypted decrypt failed: type=0x{pkt_type:02X} payload_len={len(payload)} error={e}")
        return None, None

# -- DH Handshake --------------------------------------------------------------
async def dh_handshake(reader, writer, lock):
    try:
        server_private_key = DH_PARAMETERS.generate_private_key()
        server_public_key  = server_private_key.public_key()

        params_pem = DH_PARAMETERS.parameter_bytes(
            serialization.Encoding.PEM,
            serialization.ParameterFormat.PKCS3
        )
        server_pub_pem = server_public_key.public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo
        )

        log.debug(f"DH_INIT: params_len={len(params_pem)} pub_len={len(server_pub_pem)}")

        # [params_len 2B][params_pem][pub_len 2B][pub_pem]
        payload  = struct.pack('>H', len(params_pem)) + params_pem
        payload += struct.pack('>H', len(server_pub_pem)) + server_pub_pem
        await write_packet(writer, lock, SCCP_DH_INIT, payload)
        log.info("DH_INIT sent")

        pkt_type, payload = await read_packet(reader, timeout=120.0)
        log.debug(f"DH response: type={pkt_type} payload_len={len(payload) if payload else 0}")

        if pkt_type != SCCP_DH_RESPONSE or len(payload) < 2:
            log.error(f"DH_RESPONSE bad: type={pkt_type}")
            return None

        client_pub_len = struct.unpack('>H', payload[:2])[0]
        client_pub_raw = payload[2:2 + client_pub_len]
        log.debug(f"DH_RESPONSE: client_pub_raw_len={client_pub_len} first8={client_pub_raw[:8].hex()}")

        # Raw bignum bytes from Xbox -- reconstruct DHPublicNumbers directly
        from cryptography.hazmat.primitives.asymmetric.dh import DHPublicNumbers
        client_pub_int     = int.from_bytes(client_pub_raw, 'big')
        client_pub_numbers = DHPublicNumbers(client_pub_int, DH_PARAMETERS.parameter_numbers())
        client_public_key  = client_pub_numbers.public_key(default_backend())

        # Log p, g, server pub key for cross-check with Xbox DH-P/DH-G/DH-SY
        _pn = DH_PARAMETERS.parameter_numbers()
        _pb = _pn.p.to_bytes((_pn.p.bit_length()+7)//8, "big")
        _spub = server_public_key.public_numbers().y.to_bytes((_pn.p.bit_length()+7)//8, "big")
        log.debug(f"DH-P  first8={_pb[:8].hex()} len={len(_pb)}")
        log.debug(f"DH-G  value={_pn.g}")
        log.debug(f"DH-SY first8={_spub[:8].hex()} len={len(_spub)}")
        shared_secret = server_private_key.exchange(client_public_key)
        log.debug(f"Shared secret len={len(shared_secret)} first8={shared_secret[:8].hex()}")

        session_key = derive_key(shared_secret)
        log.info(f"DH handshake complete, session_key first8={session_key[:8].hex()}")
        return session_key

    except Exception as e:
        log.error(f"DH handshake error: {e}", exc_info=True)
        return None

# -- Helpers -------------------------------------------------------------------
async def send_error(writer, lock, session_key, message):
    log.debug(f"send_error: {message}")
    msg     = message.encode('utf-8')
    payload = bytes([len(msg)]) + msg
    await write_encrypted(writer, lock, session_key, SCCP_ERROR, payload)

def pack_string8(s):
    b = s.encode('utf-8')[:255]
    return bytes([len(b)]) + b

def pack_string16(s):
    b = s.encode('utf-8')[:65535]
    return struct.pack('>H', len(b)) + b

def unpack_string8(data, offset):
    length = data[offset]
    return data[offset+1:offset+1+length].decode('utf-8', errors='replace'), offset+1+length

def unpack_string16(data, offset):
    length = struct.unpack('>H', data[offset:offset+2])[0]
    return data[offset+2:offset+2+length].decode('utf-8', errors='replace'), offset+2+length

# -- Handlers ------------------------------------------------------------------
async def handle_register(writer, lock, session_key, payload):
    try:
        username, off = unpack_string8(payload, 0)
        password, _   = unpack_string8(payload, off)
        username = username.strip()
        password = password.strip()
        log.info(f"REGISTER attempt: username={username}")

        if len(username) < 3 or len(username) > 32:
            await send_error(writer, lock, session_key, 'Username must be 3-32 characters')
            return
        if len(password) < 8:
            await send_error(writer, lock, session_key, 'Password must be at least 8 characters')
            return

        db     = get_db()
        cursor = db.cursor()

        cursor.execute("SELECT id FROM users WHERE username = %s", (username,))
        if cursor.fetchone():
            log.info(f"REGISTER fail: username taken: {username}")
            await send_error(writer, lock, session_key, 'Username already taken')
            return

        salt          = secrets.token_hex(32)
        password_hash = bcrypt.hashpw(
            password.encode('utf-8'), bcrypt.gensalt()
        ).decode('utf-8')

        cursor.execute(
            "INSERT INTO users (username, password_hash, salt) VALUES (%s, %s, %s)",
            (username, password_hash, salt)
        )
        db.commit()
        log.info(f"REGISTER success: {username}")

        await write_encrypted(writer, lock, session_key, SCCP_AUTH_OK,
            bytes([0x00]) + pack_string8('Registration successful'))

    except Exception as e:
        log.error(f"Register error: {e}", exc_info=True)
        await send_error(writer, lock, session_key, 'Registration failed')
    finally:
        cursor.close()
        db.close()

async def handle_login(writer, lock, session_key, payload):
    try:
        username, off = unpack_string8(payload, 0)
        password, _   = unpack_string8(payload, off)
        log.info(f"LOGIN attempt: username={username}")

        db     = get_db()
        cursor = db.cursor()

        cursor.execute(
            "SELECT id, password_hash FROM users WHERE username = %s AND is_banned = 0",
            (username.strip(),)
        )
        user = cursor.fetchone()

        if not user or not bcrypt.checkpw(password.encode('utf-8'), user[1].encode('utf-8')):
            log.info(f"LOGIN fail: bad credentials for {username}")
            await send_error(writer, lock, session_key, 'Invalid credentials')
            return None

        token  = secrets.token_hex(64)
        expiry = datetime.now() + timedelta(hours=24)
        cursor.execute(
            "UPDATE users SET token = %s, token_expiry = %s WHERE id = %s",
            (token, expiry, user[0])
        )
        db.commit()
        log.info(f"LOGIN success: {username} id={user[0]}")

        # [user_id 4B][token_len 1B][token][username]
        payload_out  = struct.pack('>I', user[0])
        token_bytes  = token.encode('utf-8')
        payload_out += bytes([len(token_bytes)]) + token_bytes
        payload_out += pack_string8(username)
        await write_encrypted(writer, lock, session_key, SCCP_AUTH_OK, payload_out)

        return user[0], username

    except Exception as e:
        log.error(f"Login error: {e}", exc_info=True)
        await send_error(writer, lock, session_key, 'Login failed')
        return None
    finally:
        cursor.close()
        db.close()

async def handle_room_list(writer, lock, session_key):
    try:
        db     = get_db()
        cursor = db.cursor()
        cursor.execute("SELECT id, name, type FROM rooms ORDER BY type, name")
        rows = cursor.fetchall()
        log.debug(f"room_list: {len(rows)} rooms")

        # [count 1B] then per room: [id 1B][type 1B][name]
        payload = bytes([len(rows)])
        for row in rows:
            room_type = 0x01 if row[2] == 'voice' else 0x00
            payload  += bytes([row[0], room_type]) + pack_string8(row[1])

        await write_encrypted(writer, lock, session_key, SCCP_ROOM_LIST, payload)

    except Exception as e:
        log.error(f"Room list error: {e}", exc_info=True)
        await send_error(writer, lock, session_key, 'Room list failed')
    finally:
        cursor.close()
        db.close()

async def handle_join_room(writer, lock, session_key, payload, client_id):
    if not client_id or client_id not in connected_clients:
        await send_error(writer, lock, session_key, 'Not authenticated')
        return

    room_id = payload[0]
    log.info(f"JOIN_ROOM: client={client_id} room={room_id}")

    try:
        db     = get_db()
        cursor = db.cursor()

        cursor.execute("SELECT id, name, type FROM rooms WHERE id = %s", (room_id,))
        room = cursor.fetchone()
        if not room:
            await send_error(writer, lock, session_key, 'Room not found')
            return

        connected_clients[client_id]['room'] = room_id

        cursor.execute("""
            SELECT u.username, m.content, m.sent_at
            FROM messages m
            JOIN users u ON m.user_id = u.id
            WHERE m.room_id = %s AND m.is_deleted = 0
            ORDER BY m.sent_at DESC
            LIMIT 50
        """, (room_id,))
        rows = list(reversed(cursor.fetchall()))

        room_type = 0x01 if room[2] == 'voice' else 0x00
        out  = bytes([room[0], room_type])
        out += pack_string8(room[1])
        out += bytes([len(rows)])
        for row in rows:
            ts   = row[2].strftime('%H:%M')
            out += pack_string8(row[0])
            out += pack_string16(row[1])
            out += pack_string8(ts)

        await write_encrypted(writer, lock, session_key, SCCP_ROOM_INFO, out)

    except Exception as e:
        log.error(f"Join room error: {e}", exc_info=True)
        await send_error(writer, lock, session_key, 'Join room failed')
    finally:
        cursor.close()
        db.close()


# -- scene_bot -----------------------------------------------------------------

async def bot_reply(room_id: int, text: str, target_client_id: int = None):
    """Send a message from scene_bot. If target_client_id is set, DM only that client."""
    ts        = datetime.now().strftime('%H:%M')
    payload   = bytes([room_id])
    payload  += pack_string8(SCENE_BOT_USERNAME)
    payload  += pack_string16(text)
    payload  += pack_string8(ts)

    targets = {}
    if target_client_id and target_client_id in connected_clients:
        targets = {target_client_id: connected_clients[target_client_id]}
    else:
        targets = {uid: c for uid, c in connected_clients.items()
                   if c['room'] == room_id}

    for uid, client in targets.items():
        try:
            await write_encrypted(client['writer'], client['lock'],
                                  client['session_key'], SCCP_MSG_RECV, payload)
        except Exception:
            pass


async def handle_bot_command(content: str, room_id: int, client_id: int):
    """Parse and execute a bot command. Returns True if command was handled."""
    parts   = content.strip().split()
    if not parts or not parts[0].startswith('/'):
        return False

    cmd  = parts[0].lower()
    args = parts[1:]
    username = connected_clients.get(client_id, {}).get('username', '?')

    # -- Determine caller role -------------------------------------------------
    caller_role = 'user'
    try:
        db     = get_db()
        cursor = db.cursor()
        cursor.execute("SELECT role FROM users WHERE id = %s", (client_id,))
        row = cursor.fetchone()
        if row:
            caller_role = row[0]
    except Exception:
        pass
    finally:
        try: cursor.close()
        except: pass
        try: db.close()
        except: pass

    is_mod = caller_role in ('moderator', 'admin', 'superadmin')

    # -- /help -----------------------------------------------------------------
    if cmd == '/help':
        sub = args[0].lower() if args else ''
        if sub == 'emoji':
            lines = ['[scene_bot] Available emoji -- use :token: in any message:']
            row = []
            for name in EMOJI_LIST:
                row.append(f'{name}')
                if len(row) == 3:
                    lines.append('  ' + '   '.join(row))
                    row = []
            if row:
                lines.append('  ' + '   '.join(row))
            lines.append('Example: Hello :smile: world :fire:')
            await bot_reply(room_id, '\n'.join(lines), client_id)
        elif sub == 'rooms':
            await bot_reply(room_id, (
                '[scene_bot] Room Help:\n'
                '  /rooms         - List all rooms\n'
                '  /online        - Show connected users and their rooms'
            ), client_id)
        elif sub == 'mod' and is_mod:
            await bot_reply(room_id, (
                '[scene_bot] Moderator Commands:\n'
                '  /kick <user>     - Disconnect a user\n'
                '  /ban <user>      - Ban a user\n'
                '  /unban <user>    - Unban a user\n'
                '  /mute <user>     - Mute a user (clears on restart)\n'
                '  /unmute <user>   - Unmute a user\n'
                '  /announce <msg>  - Broadcast to all rooms\n'
                '  /room list       - List rooms with IDs\n'
                '  /room create <name> - Create a new room\n'
                '  /room delete <id>   - Delete a room'
            ), client_id)
        else:
            mod_hint = '  /help mod      - Moderator commands\n' if is_mod else ''
            await bot_reply(room_id, (
                '[scene_bot] SceneChat Help:\n'
                '  /help          - This message\n'
                '  /help emoji    - Emoji help\n'
                '  /help rooms    - Room help\n'
                + mod_hint +
                '  /online        - Show connected users\n'
                '  /rooms         - List all rooms\n'
                '  /emoji         - List emoji tokens\n'
                '  /me <text>     - Action message\n'
                '  /ping          - Check server status\n'
                '  /time          - Show server time\n'
                '  /version       - Show server version'
            ), client_id)
        return True

    # -- /ping -----------------------------------------------------------------
    if cmd == '/ping':
        await bot_reply(room_id, '[scene_bot] Pong! Server is alive.', client_id)
        return True

    # -- /time -----------------------------------------------------------------
    if cmd == '/time':
        now = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        await bot_reply(room_id, f'[scene_bot] Server time: {now}', client_id)
        return True

    # -- /version --------------------------------------------------------------
    if cmd == '/version':
        await bot_reply(room_id,
            '[scene_bot] SceneChat Server v1.0 | Protocol SCCP | Team Resurgent / Darkone83',
            client_id)
        return True

    # -- /online ---------------------------------------------------------------
    if cmd == '/online':
        lines = ['[scene_bot] Connected users:']
        for uid, c in connected_clients.items():
            room = c.get('room', '?')
            lines.append(f'  {c["username"]} -> room {room}')
        if len(lines) == 1:
            lines.append('  Nobody else is connected.')
        await bot_reply(room_id, '\n'.join(lines), client_id)
        return True

    # -- /rooms ----------------------------------------------------------------
    if cmd == '/rooms':
        try:
            db     = get_db()
            cursor = db.cursor()
            cursor.execute("SELECT id, name, type FROM rooms ORDER BY type, name")
            rows = cursor.fetchall()
            lines = ['[scene_bot] Available rooms:']
            for row in rows:
                icon = '[voice]' if row[2] == 'voice' else '[text]'
                lines.append(f'  {icon} #{row[1]} (id:{row[0]})')
            await bot_reply(room_id, '\n'.join(lines), client_id)
        except Exception as e:
            await bot_reply(room_id, f'[scene_bot] Error: {e}', client_id)
        finally:
            try: cursor.close()
            except: pass
            try: db.close()
            except: pass
        return True

    # -- /emoji ----------------------------------------------------------------
    if cmd == '/emoji':
        chunks = []
        line   = '[scene_bot] Emoji tokens -- wrap name with : to use e.g. :smile:\n'
        for i, name in enumerate(EMOJI_LIST):
            line += f'  {name}'
            if (i + 1) % 4 == 0:
                chunks.append(line)
                line = ''
        if line:
            chunks.append(line)
        await bot_reply(room_id, '\n'.join(chunks), client_id)
        return True

    # -- /me -------------------------------------------------------------------
    if cmd == '/me':
        if not args:
            await bot_reply(room_id, '[scene_bot] Usage: /me <action text>', client_id)
            return True
        action_text = ' '.join(args)
        # Broadcast the action as a regular message styled as emote
        ts      = datetime.now().strftime('%H:%M')
        payload = bytes([room_id])
        payload += pack_string8(username)
        payload += pack_string16(f'* {username} {action_text}')
        payload += pack_string8(ts)
        for uid, client in list(connected_clients.items()):
            if client['room'] == room_id:
                try:
                    await write_encrypted(client['writer'], client['lock'],
                                          client['session_key'], SCCP_MSG_RECV, payload)
                except Exception:
                    pass
        # Also save to DB
        try:
            db     = get_db()
            cursor = db.cursor()
            cursor.execute(
                "INSERT INTO messages (room_id, user_id, content) VALUES (%s, %s, %s)",
                (room_id, client_id, f'* {username} {action_text}')
            )
            db.commit()
        except Exception:
            pass
        finally:
            try: cursor.close()
            except: pass
            try: db.close()
            except: pass
        return True

    # -- Moderator commands ----------------------------------------------------
    if not is_mod:
        if cmd in ('/kick','/ban','/unban','/mute','/unmute','/announce','/room'):
            await bot_reply(room_id,
                '[scene_bot] Permission denied. Moderator access required.', client_id)
            return True
        return False  # unknown command, let it pass through as normal message

    # -- /kick -----------------------------------------------------------------
    if cmd == '/kick':
        if not args:
            await bot_reply(room_id, '[scene_bot] Usage: /kick <username>', client_id)
            return True
        target_name = args[0]
        target_id   = next((uid for uid, c in connected_clients.items()
                            if c['username'].lower() == target_name.lower()), None)
        if not target_id:
            await bot_reply(room_id, f'[scene_bot] User {target_name} not found online.', client_id)
            return True
        await bot_reply(room_id,
            f'[scene_bot] {target_name} has been kicked by {username}.', None)
        try:
            connected_clients[target_id]['writer'].close()
        except Exception:
            pass
        log.info(f'BOT: {username} kicked {target_name}')
        return True

    # -- /ban ------------------------------------------------------------------
    if cmd == '/ban':
        if not args:
            await bot_reply(room_id, '[scene_bot] Usage: /ban <username>', client_id)
            return True
        target_name = args[0]
        try:
            db     = get_db()
            cursor = db.cursor()
            cursor.execute("UPDATE users SET is_banned = 1 WHERE username = %s", (target_name,))
            db.commit()
            affected = cursor.rowcount
            if affected:
                await bot_reply(room_id,
                    f'[scene_bot] {target_name} has been banned by {username}.', None)
                # Kick if online
                target_id = next((uid for uid, c in connected_clients.items()
                                  if c['username'].lower() == target_name.lower()), None)
                if target_id:
                    try: connected_clients[target_id]['writer'].close()
                    except Exception: pass
            else:
                await bot_reply(room_id, f'[scene_bot] User {target_name} not found.', client_id)
        except Exception as e:
            await bot_reply(room_id, f'[scene_bot] Error: {e}', client_id)
        finally:
            try: cursor.close()
            except: pass
            try: db.close()
            except: pass
        log.info(f'BOT: {username} banned {target_name}')
        return True

    # -- /unban ----------------------------------------------------------------
    if cmd == '/unban':
        if not args:
            await bot_reply(room_id, '[scene_bot] Usage: /unban <username>', client_id)
            return True
        target_name = args[0]
        try:
            db     = get_db()
            cursor = db.cursor()
            cursor.execute("UPDATE users SET is_banned = 0 WHERE username = %s", (target_name,))
            db.commit()
            if cursor.rowcount:
                await bot_reply(room_id,
                    f'[scene_bot] {target_name} has been unbanned by {username}.', client_id)
            else:
                await bot_reply(room_id, f'[scene_bot] User {target_name} not found.', client_id)
        except Exception as e:
            await bot_reply(room_id, f'[scene_bot] Error: {e}', client_id)
        finally:
            try: cursor.close()
            except: pass
            try: db.close()
            except: pass
        return True

    # -- /mute -----------------------------------------------------------------
    if cmd == '/mute':
        if not args:
            await bot_reply(room_id, '[scene_bot] Usage: /mute <username>', client_id)
            return True
        target_name = args[0].lower()
        muted_users.add(target_name)
        await bot_reply(room_id,
            f'[scene_bot] {args[0]} has been muted by {username}.', None)
        log.info(f'BOT: {username} muted {target_name}')
        return True

    # -- /unmute ---------------------------------------------------------------
    if cmd == '/unmute':
        if not args:
            await bot_reply(room_id, '[scene_bot] Usage: /unmute <username>', client_id)
            return True
        target_name = args[0].lower()
        muted_users.discard(target_name)
        await bot_reply(room_id,
            f'[scene_bot] {args[0]} has been unmuted by {username}.', None)
        return True

    # -- /announce -------------------------------------------------------------
    if cmd == '/announce':
        if not args:
            await bot_reply(room_id, '[scene_bot] Usage: /announce <message>', client_id)
            return True
        msg = ' '.join(args)
        ts  = datetime.now().strftime('%H:%M')
        for uid, client in list(connected_clients.items()):
            r   = client.get('room', room_id)
            pkt = bytes([r]) + pack_string8(SCENE_BOT_USERNAME) + pack_string16(f'[ANNOUNCE] {msg}') + pack_string8(ts)
            try:
                await write_encrypted(client['writer'], client['lock'],
                                      client['session_key'], SCCP_MSG_RECV, pkt)
            except Exception:
                pass
        log.info(f'BOT: {username} announced: {msg}')
        return True

    # -- /room -----------------------------------------------------------------
    if cmd == '/room':
        sub = args[0].lower() if args else ''
        if sub == 'list':
            return await handle_bot_command('/rooms', room_id, client_id)
        elif sub == 'create' and len(args) >= 2:
            room_name = ' '.join(args[1:])
            try:
                db     = get_db()
                cursor = db.cursor()
                cursor.execute(
                    "INSERT INTO rooms (name, type) VALUES (%s, 'text')", (room_name,))
                db.commit()
                await bot_reply(room_id,
                    f'[scene_bot] Room #{room_name} created.', client_id)
            except Exception as e:
                await bot_reply(room_id, f'[scene_bot] Error: {e}', client_id)
            finally:
                try: cursor.close()
                except: pass
                try: db.close()
                except: pass
        elif sub == 'delete' and len(args) >= 2:
            try:
                rid = int(args[1])
                db     = get_db()
                cursor = db.cursor()
                cursor.execute("DELETE FROM rooms WHERE id = %s", (rid,))
                db.commit()
                await bot_reply(room_id,
                    f'[scene_bot] Room {rid} deleted.', client_id)
            except Exception as e:
                await bot_reply(room_id, f'[scene_bot] Error: {e}', client_id)
            finally:
                try: cursor.close()
                except: pass
                try: db.close()
                except: pass
        else:
            await bot_reply(room_id,
                '[scene_bot] Usage: /room list | /room create <name> | /room delete <id>',
                client_id)
        return True

    # Unknown command
    await bot_reply(room_id,
        f'[scene_bot] Unknown command: {cmd}. Type /help for a list.', client_id)
    return True

async def handle_message(writer, lock, session_key, payload, client_id):
    if not client_id or client_id not in connected_clients:
        await send_error(writer, lock, session_key, 'Not authenticated')
        return

    room_id     = payload[0]
    content, _  = unpack_string16(payload, 1)
    content     = content.strip()
    log.info(f"MESSAGE: client={client_id} room={room_id} len={len(content)}")

    if not content or len(content) > 500:
        await send_error(writer, lock, session_key, 'Invalid message')
        return

    # Check mute
    sender_name = connected_clients.get(client_id, {}).get('username', '').lower()
    if sender_name in muted_users:
        await bot_reply(room_id, '[scene_bot] You are muted.', client_id)
        return

    # Intercept bot commands
    if content.startswith('/'):
        await handle_bot_command(content, room_id, client_id)
        return

    try:
        db     = get_db()
        cursor = db.cursor()
        cursor.execute(
            "INSERT INTO messages (room_id, user_id, content) VALUES (%s, %s, %s)",
            (room_id, client_id, content)
        )
        db.commit()

        username = connected_clients[client_id]['username']
        ts       = datetime.now().strftime('%H:%M')

        broadcast  = bytes([room_id])
        broadcast += pack_string8(username)
        broadcast += pack_string16(content)
        broadcast += pack_string8(ts)

        sent = 0
        for uid, client in list(connected_clients.items()):
            if client['room'] == room_id:
                try:
                    await write_encrypted(
                        client['writer'], client['lock'],
                        client['session_key'],
                        SCCP_MSG_RECV, broadcast
                    )
                    sent += 1
                except Exception as be:
                    log.warning(f"broadcast to {uid} failed: {be}")
        log.debug(f"MESSAGE broadcast to {sent} clients")

    except Exception as e:
        log.error(f"Message error: {e}", exc_info=True)
        await send_error(writer, lock, session_key, 'Message failed')
    finally:
        cursor.close()
        db.close()

# -- Client handler ------------------------------------------------------------
async def handle_client(reader, writer):
    addr      = writer.get_extra_info('peername')
    client_id = None
    lock      = asyncio.Lock()

    log.info(f"Connection from {addr}")

    session_key = await dh_handshake(reader, writer, lock)
    if not session_key:
        log.error(f"Handshake failed from {addr}")
        writer.close()
        return

    log.info(f"Handshake OK from {addr}, entering main loop")

    try:
        while True:
            pkt_type, payload = await read_encrypted(reader, session_key)
            if pkt_type is None:
                log.info(f"Client {addr} disconnected (read_encrypted returned None)")
                break

            log.debug(f"Recv pkt_type=0x{pkt_type:02X} from {addr}")

            if pkt_type == SCCP_REGISTER:
                await handle_register(writer, lock, session_key, payload)

            elif pkt_type == SCCP_LOGIN:
                result = await handle_login(writer, lock, session_key, payload)
                if result:
                    client_id, username = result
                    connected_clients[client_id] = {
                        'writer':      writer,
                        'lock':        lock,
                        'username':    username,
                        'session_key': session_key,
                        'room':        None
                    }

            elif pkt_type == SCCP_ROOM_LIST:
                await handle_room_list(writer, lock, session_key)

            elif pkt_type == SCCP_JOIN_ROOM:
                await handle_join_room(writer, lock, session_key, payload, client_id)

            elif pkt_type == SCCP_MESSAGE:
                await handle_message(writer, lock, session_key, payload, client_id)

            elif pkt_type == SCCP_PING:
                log.debug(f"PING from {addr}, sending PONG")
                await write_packet(writer, lock, SCCP_PONG)

            else:
                log.warning(f"Unknown pkt_type=0x{pkt_type:02X} from {addr}")
                await send_error(writer, lock, session_key, 'Unknown packet')

    except Exception as e:
        log.error(f"Client error {addr}: {e}", exc_info=True)
    finally:
        if client_id and client_id in connected_clients:
            # Persist last_seen and last_room before removing client
            try:
                last_room = connected_clients[client_id].get('room')
                db     = get_db()
                cursor = db.cursor()
                cursor.execute(
                    "UPDATE users SET last_seen = NOW(), last_room = %s WHERE id = %s",
                    (last_room, client_id)
                )
                db.commit()
            except Exception as _pe:
                log.warning(f"Could not update last_seen for {client_id}: {_pe}")
            finally:
                try: cursor.close()
                except: pass
                try: db.close()
                except: pass
            del connected_clients[client_id]
        log.info(f"Disconnected: {addr}")
        writer.close()

# -- Main ----------------------------------------------------------------------

# ---------------------------------------------------------------------------
#  Admin message broadcast  (called from internal HTTP listener)
# ---------------------------------------------------------------------------

import json as _json

async def admin_broadcast(room_id, content):
    """Save an admin message to DB and broadcast to connected clients."""
    content = content.strip()
    if not content or len(content) > 500:
        return False, 'Invalid content'
    username = '[Admin]'
    ts = datetime.now().strftime('%H:%M')
    try:
        db     = get_db()
        cursor = db.cursor()
        cursor.execute("SELECT id FROM users WHERE username = 'Admin' LIMIT 1")
        row = cursor.fetchone()
        if row:
            admin_uid = row[0]
        else:
            cursor.execute(
                "INSERT INTO users (username, password_hash, salt, role) VALUES ('Admin', '', '', 'admin')"
            )
            admin_uid = cursor.lastrowid
        cursor.execute(
            "INSERT INTO messages (room_id, user_id, content) VALUES (%s, %s, %s)",
            (room_id, admin_uid, content)
        )
        db.commit()
    except Exception as e:
        return False, str(e)
    finally:
        try:
            cursor.close()
            db.close()
        except Exception:
            pass

    broadcast  = bytes([room_id])
    broadcast += pack_string8(username)
    broadcast += pack_string16(content)
    broadcast += pack_string8(ts)

    sent = 0
    for uid, client in list(connected_clients.items()):
        if client.get('room') == room_id:
            try:
                await write_encrypted(
                    client['writer'], client['lock'],
                    client['session_key'],
                    SCCP_MSG_RECV, broadcast
                )
                sent += 1
            except Exception as be:
                log.warning(f'admin broadcast to {uid} failed: {be}')
    log.info(f'ADMIN_MSG room={room_id} broadcast={sent}')
    return True, f'Sent to {sent} clients'


async def _handle_admin_http(reader, writer):
    """Minimal asyncio HTTP handler for admin API (localhost:8951)."""
    CRLF = '\r\n'
    try:
        head = await asyncio.wait_for(reader.read(4096), timeout=5)
        text = head.decode('utf-8', errors='replace')

        # Split request line
        first_line = text.split('\r\n')[0] if '\r\n' in text else text.split('\n')[0]
        parts = first_line.split(' ')
        method = parts[0] if len(parts) > 0 else ''
        path   = parts[1] if len(parts) > 1 else ''

        if method == 'POST' and path == '/admin/send':
            sep   = '\r\n\r\n' if '\r\n\r\n' in text else '\n\n'
            body  = text.split(sep, 1)[1] if sep in text else ''
            data  = _json.loads(body)
            ok, msg = await admin_broadcast(int(data['room_id']), str(data['content']))
            resp_body = _json.dumps({'ok': ok, 'msg': msg})
            status    = '200 OK' if ok else '400 Bad Request'
        else:
            resp_body = _json.dumps({'ok': False, 'msg': 'Not found'})
            status    = '404 Not Found'

        response = (
            'HTTP/1.1 ' + status + CRLF +
            'Content-Type: application/json' + CRLF +
            'Content-Length: ' + str(len(resp_body)) + CRLF +
            CRLF +
            resp_body
        )
        writer.write(response.encode())
        await writer.drain()
    except Exception as e:
        log.warning(f'admin HTTP handler error: {e}')
    finally:
        try:
            writer.close()
        except Exception:
            pass

async def main():
    server       = await asyncio.start_server(handle_client, '0.0.0.0', 8943)
    admin_server = await asyncio.start_server(_handle_admin_http, '127.0.0.1', 8951)
    log.info("SceneChat server listening on port 8943")
    log.info("Admin API listening on localhost:8951")
    async with server:
        async with admin_server:
            await asyncio.gather(
                server.serve_forever(),
                admin_server.serve_forever()
            )

if __name__ == '__main__':
    asyncio.run(main())