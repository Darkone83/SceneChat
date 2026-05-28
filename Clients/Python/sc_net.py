"""
sc_net.py -- SceneChat Python client network worker.
Runs asyncio in a QThread. Communicates with the UI via Qt signals.
"""

import asyncio
import struct
import os
import json
from pathlib import Path

from PySide6.QtCore import QThread, Signal

from cryptography.hazmat.primitives.asymmetric.dh import DHPublicNumbers
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.backends import default_backend

# ---------------------------------------------------------------------------
#  SCCP packet types
# ---------------------------------------------------------------------------
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

CREDS_FILE    = Path(__file__).parent / 'creds.json'
PING_INTERVAL = 10.0

# ---------------------------------------------------------------------------
#  Crypto
# ---------------------------------------------------------------------------

def _derive_key(shared_secret: bytes) -> bytes:
    hkdf = HKDF(
        algorithm=hashes.SHA256(),
        length=32,
        salt=bytes(32),
        info=b'scenechat-session',
        backend=default_backend()
    )
    return hkdf.derive(shared_secret)

def _encrypt(session_key: bytes, plaintext: bytes) -> bytes:
    nonce = os.urandom(12)
    chacha = ChaCha20Poly1305(session_key)
    return nonce + chacha.encrypt(nonce, plaintext, None)

def _decrypt(session_key: bytes, data: bytes) -> bytes:
    nonce = data[:12]
    chacha = ChaCha20Poly1305(session_key)
    return chacha.decrypt(nonce, data[12:], None)

# ---------------------------------------------------------------------------
#  Packet framing
# ---------------------------------------------------------------------------

def _pack_string8(s: str) -> bytes:
    b = s.encode('utf-8')[:255]
    return bytes([len(b)]) + b

def _pack_string16(s: str) -> bytes:
    b = s.encode('utf-8')[:65535]
    return struct.pack('>H', len(b)) + b

def _unpack_string8(data: bytes, offset: int):
    length = data[offset]
    return data[offset+1:offset+1+length].decode('utf-8', errors='replace'), offset+1+length

def _unpack_string16(data: bytes, offset: int):
    length = struct.unpack('>H', data[offset:offset+2])[0]
    return data[offset+2:offset+2+length].decode('utf-8', errors='replace'), offset+2+length

async def _read_packet(reader):
    try:
        header = await asyncio.wait_for(reader.readexactly(2), timeout=300.0)
        total_len = struct.unpack('>H', header)[0]
        if total_len < 3:
            return None, None
        rest = await asyncio.wait_for(reader.readexactly(total_len - 2), timeout=120.0)
        return rest[0], bytes(rest[1:])
    except Exception:
        return None, None

async def _write_packet(writer, pkt_type: int, payload: bytes = b''):
    data = bytes([pkt_type]) + payload
    writer.write(struct.pack('>H', len(data) + 2) + data)
    await writer.drain()

async def _write_encrypted(writer, session_key: bytes, pkt_type: int, payload: bytes = b''):
    encrypted = _encrypt(session_key, payload)
    await _write_packet(writer, pkt_type, encrypted)

async def _read_encrypted(reader, session_key: bytes):
    pkt_type, payload = await _read_packet(reader)
    if pkt_type is None or len(payload) < 12:
        return None, None
    try:
        plaintext = _decrypt(session_key, payload)
        return pkt_type, plaintext
    except Exception:
        return None, None

# ---------------------------------------------------------------------------
#  Credentials
# ---------------------------------------------------------------------------

def load_creds():
    try:
        if CREDS_FILE.exists():
            return json.loads(CREDS_FILE.read_text())
    except Exception:
        pass
    return {}

def save_creds(server: str, username: str, password: str):
    try:
        CREDS_FILE.write_text(json.dumps({
            'server': server, 'username': username, 'password': password
        }))
    except Exception:
        pass

def clear_creds():
    try:
        CREDS_FILE.unlink(missing_ok=True)
    except Exception:
        pass

# ---------------------------------------------------------------------------
#  Network worker
# ---------------------------------------------------------------------------

class ChatWorker(QThread):
    sig_connected    = Signal()
    sig_auth_ok      = Signal(int, str, str)
    sig_auth_fail    = Signal(str)
    sig_room_list    = Signal(list)
    sig_room_joined  = Signal(int, str, list)
    sig_message      = Signal(int, str, str, str)
    sig_error        = Signal(str)
    sig_disconnected = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._server           = ''
        self._port             = 8943
        self._loop             = None
        self._writer           = None
        self._session_key      = None
        self._stop             = False
        self._pending          = []
        self._pending_register = False

    def configure(self, server: str, port: int = 8943):
        self._server = server
        self._port   = port

    def send_login(self, username: str, password: str):
        self._schedule(self._do_login, username, password)

    def send_register(self, username: str, password: str):
        self._schedule(self._do_register, username, password)

    def send_join_room(self, room_id: int):
        self._schedule(self._do_join_room, room_id)

    def send_message(self, room_id: int, content: str):
        self._schedule(self._do_send_message, room_id, content)

    def send_disconnect(self):
        self._stop = True
        self._schedule(self._do_disconnect)

    def _schedule(self, coro_fn, *args):
        if self._loop and self._loop.is_running():
            self._loop.call_soon_threadsafe(
                lambda: self._loop.create_task(coro_fn(*args))
            )
        else:
            self._pending.append((coro_fn, args))

    def run(self):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._main())
        except Exception as e:
            self.sig_disconnected.emit(str(e))
        finally:
            self._loop.close()

    async def _main(self):
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self._server, self._port),
                timeout=10.0
            )
        except Exception as e:
            self.sig_disconnected.emit(f'Connection failed: {e}')
            return

        self._writer = writer

        session_key = await self._dh_handshake(reader, writer)
        if not session_key:
            self.sig_disconnected.emit('Handshake failed')
            writer.close()
            return

        self._session_key = session_key
        self.sig_connected.emit()

        for fn, args in self._pending:
            self._loop.create_task(fn(*args))
        self._pending.clear()

        ping_task = self._loop.create_task(self._ping_loop(writer))

        try:
            while not self._stop:
                # Read raw packet first -- PONG is sent unencrypted by server
                raw_type, raw_payload = await _read_packet(reader)
                if raw_type is None:
                    break
                if raw_type == SCCP_PONG:
                    continue  # unencrypted PONG -- just ignore and keep going
                # All other packets are encrypted
                if len(raw_payload) < 12:
                    break
                try:
                    plaintext = _decrypt(session_key, raw_payload)
                except Exception:
                    break
                await self._dispatch(raw_type, plaintext, writer)
        except Exception:
            pass
        finally:
            ping_task.cancel()
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
            if not self._stop:
                self.sig_disconnected.emit('Connection lost')

    # ── DH Handshake ─────────────────────────────────────────────────────────

    async def _dh_handshake(self, reader, writer) -> bytes:
        try:
            pkt_type, payload = await _read_packet(reader)
            if pkt_type != SCCP_DH_INIT or not payload:
                return None

            pos        = 0
            params_len = struct.unpack_from('>H', payload, pos)[0]; pos += 2
            params_pem = payload[pos:pos+params_len];               pos += params_len
            pub_len    = struct.unpack_from('>H', payload, pos)[0]; pos += 2
            pub_pem    = payload[pos:pos+pub_len]

            parameters  = serialization.load_pem_parameters(params_pem, backend=default_backend())
            server_pub  = serialization.load_pem_public_key(pub_pem, backend=default_backend())
            client_priv = parameters.generate_private_key()
            client_pub_y = client_priv.public_key().public_numbers().y
            key_bytes    = (parameters.parameter_numbers().p.bit_length() + 7) // 8
            client_pub_raw = client_pub_y.to_bytes(key_bytes, 'big')

            resp_payload = struct.pack('>H', len(client_pub_raw)) + client_pub_raw
            await _write_packet(writer, SCCP_DH_RESPONSE, resp_payload)

            shared = client_priv.exchange(server_pub)
            return _derive_key(shared)

        except Exception as e:
            self.sig_error.emit(f'DH error: {e}')
            return None

    # ── Dispatch ──────────────────────────────────────────────────────────────

    async def _dispatch(self, pkt_type: int, payload: bytes, writer):
        if pkt_type == SCCP_AUTH_OK:
            await self._on_auth_ok(payload)
        elif pkt_type in (SCCP_AUTH_FAIL, SCCP_ERROR):
            msg, _ = _unpack_string8(payload, 0)
            self.sig_auth_fail.emit(msg)
        elif pkt_type == SCCP_ROOM_LIST:
            self._on_room_list(payload)
        elif pkt_type == SCCP_ROOM_INFO:
            self._on_room_info(payload)
        elif pkt_type == SCCP_MSG_RECV:
            self._on_msg_recv(payload)
        elif pkt_type == SCCP_PONG:
            pass

    async def _on_auth_ok(self, payload: bytes):
        try:
            if self._pending_register:
                self._pending_register = False
                self.sig_auth_ok.emit(0, '', '')
                return
            pos = 0
            user_id = struct.unpack_from('>I', payload, pos)[0]; pos += 4
            token, pos   = _unpack_string8(payload, pos)
            username, _  = _unpack_string8(payload, pos)
            self.sig_auth_ok.emit(user_id, username, token)
            await _write_encrypted(self._writer, self._session_key, SCCP_ROOM_LIST)
        except Exception as e:
            self.sig_error.emit(f'AUTH_OK parse error: {e}')

    def _on_room_list(self, payload: bytes):
        try:
            count = payload[0]; pos = 1
            rooms = []
            for _ in range(count):
                room_id   = payload[pos]; pos += 1
                room_type = payload[pos]; pos += 1
                name, pos = _unpack_string8(payload, pos)
                rooms.append((room_id, name, room_type))
            self.sig_room_list.emit(rooms)
        except Exception as e:
            self.sig_error.emit(f'ROOM_LIST parse error: {e}')

    def _on_room_info(self, payload: bytes):
        try:
            pos     = 0
            room_id = payload[pos]; pos += 1
            pos    += 1  # type
            name, pos = _unpack_string8(payload, pos)
            count  = payload[pos]; pos += 1
            history = []
            for _ in range(count):
                username, pos = _unpack_string8(payload, pos)
                content,  pos = _unpack_string16(payload, pos)
                ts,       pos = _unpack_string8(payload, pos)
                history.append({'username': username, 'content': content, 'ts': ts})
            self.sig_room_joined.emit(room_id, name, history)
        except Exception as e:
            self.sig_error.emit(f'ROOM_INFO parse error: {e}')

    def _on_msg_recv(self, payload: bytes):
        try:
            pos = 0
            room_id       = payload[pos]; pos += 1
            username, pos = _unpack_string8(payload, pos)
            content,  pos = _unpack_string16(payload, pos)
            ts,       _   = _unpack_string8(payload, pos)
            self.sig_message.emit(room_id, username, content, ts)
        except Exception as e:
            self.sig_error.emit(f'MSG_RECV parse error: {e}')

    # ── Send ──────────────────────────────────────────────────────────────────

    async def _do_login(self, username: str, password: str):
        self._pending_register = False
        payload = _pack_string8(username) + _pack_string8(password)
        await _write_encrypted(self._writer, self._session_key, SCCP_LOGIN, payload)

    async def _do_register(self, username: str, password: str):
        self._pending_register = True
        payload = _pack_string8(username) + _pack_string8(password)
        await _write_encrypted(self._writer, self._session_key, SCCP_REGISTER, payload)

    async def _do_join_room(self, room_id: int):
        await _write_encrypted(self._writer, self._session_key, SCCP_JOIN_ROOM, bytes([room_id]))

    async def _do_send_message(self, room_id: int, content: str):
        payload = bytes([room_id]) + _pack_string16(content)
        await _write_encrypted(self._writer, self._session_key, SCCP_MESSAGE, payload)

    async def _do_disconnect(self):
        if self._writer and self._session_key:
            try:
                await _write_encrypted(self._writer, self._session_key, SCCP_DISCONNECT)
            except Exception:
                pass

    async def _ping_loop(self, writer):
        try:
            while not self._stop:
                await asyncio.sleep(PING_INTERVAL)
                if self._session_key:
                    await _write_encrypted(writer, self._session_key, SCCP_PING)
        except asyncio.CancelledError:
            pass
        except Exception:
            pass