import asyncio
import struct
import mysql.connector
from datetime import datetime

DB_CONFIG = {
    'host': 'localhost',
    'user': 'scenechat',
    'password': 'XbSceneChat01!',
    'database': 'scenechat'
}

VOICE_PORT = 7800
MAX_PACKET_SIZE = 1024

# Active voice clients
# user_id -> {addr, room_id, slot, last_seen}
voice_clients = {}

# room_id -> set of user_ids
voice_rooms = {}

def get_db():
    return mysql.connector.connect(**DB_CONFIG)

def validate_user(user_id):
    """Validate user_id has an active non-expired session"""
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("""
            SELECT id FROM users
            WHERE id = %s
            AND is_banned = 0
            AND token_expiry > NOW()
        """, (user_id,))
        result = cursor.fetchone()
        return result is not None
    except Exception as e:
        print(f"[-] User validation error: {e}")
        return False
    finally:
        cursor.close()
        db.close()

class VoiceServerProtocol:
    def __init__(self):
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport
        print(f"[*] Voice server listening on UDP port {VOICE_PORT}")

    def datagram_received(self, data, addr):
        try:
            # Minimum header: [user_id 4B][room_id 1B][slot 1B] = 6 bytes
            if len(data) < 6:
                return

            user_id = struct.unpack('>I', data[:4])[0]
            room_id = data[4]
            slot    = data[5]
            payload = data[6:]

            # Join packet = empty payload
            if len(payload) == 0:
                self.handle_join(user_id, room_id, slot, addr)
                return

            # Audio packet -- must already be registered
            if user_id not in voice_clients:
                return

            client = voice_clients[user_id]
            client['last_seen'] = datetime.now()
            client['addr'] = addr

            self.relay_audio(user_id, room_id, slot, payload)

        except Exception as e:
            print(f"[-] Datagram error: {e}")

    def handle_join(self, user_id, room_id, slot, addr):
        if not validate_user(user_id):
            print(f"[-] Invalid user_id {user_id} from {addr}")
            return

        voice_clients[user_id] = {
            'addr':      addr,
            'room_id':   room_id,
            'slot':      slot,
            'last_seen': datetime.now()
        }

        if room_id not in voice_rooms:
            voice_rooms[room_id] = set()
        voice_rooms[room_id].add(user_id)

        print(f"[+] Voice join: user_id={user_id} room={room_id} slot={slot} addr={addr}")

        # Acknowledge join
        self.transport.sendto(bytes([0xFF]), addr)

    def relay_audio(self, sender_uid, room_id, slot, payload):
        if room_id not in voice_rooms:
            return

        # Relay packet: [slot 1B][adpcm payload]
        relay_packet = bytes([slot]) + payload

        for uid in voice_rooms[room_id]:
            if uid == sender_uid:
                continue
            client = voice_clients.get(uid)
            if not client:
                continue
            try:
                self.transport.sendto(relay_packet, client['addr'])
            except Exception as e:
                print(f"[-] Relay error: {e}")

    def error_received(self, exc):
        print(f"[-] Voice server error: {exc}")

    def connection_lost(self, exc):
        print(f"[-] Voice server connection lost: {exc}")

async def cleanup_stale_clients():
    while True:
        await asyncio.sleep(30)
        now = datetime.now()
        stale = []

        for uid, client in voice_clients.items():
            if (now - client['last_seen']).total_seconds() > 10:
                stale.append(uid)

        for uid in stale:
            client = voice_clients[uid]
            room_id = client['room_id']
            if room_id in voice_rooms:
                voice_rooms[room_id].discard(uid)
                if not voice_rooms[room_id]:
                    del voice_rooms[room_id]
            del voice_clients[uid]
            print(f"[-] Removed stale voice client user_id={uid}")

async def main():
    print(f"[*] SceneChat voice server starting on UDP port {VOICE_PORT}")
    loop = asyncio.get_event_loop()
    transport, protocol = await loop.create_datagram_endpoint(
        VoiceServerProtocol,
        local_addr=('0.0.0.0', VOICE_PORT)
    )
    try:
        await asyncio.gather(asyncio.Future(), cleanup_stale_clients())
    finally:
        transport.close()

if __name__ == "__main__":
    asyncio.run(main())