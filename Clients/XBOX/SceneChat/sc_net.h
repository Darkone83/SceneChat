#ifndef SCENECHAT_NET_H
#define SCENECHAT_NET_H
/*---------------------------------------------------------------------------
    sc_net.h -- SceneChat TCP network layer for Xbox.

    Async connect state machine:
        SC_Net_ConnectBegin()  -- kick off non-blocking connect
        SC_Net_ConnectPoll()   -- call every frame: 0=working 1=done -1=fail
        SC_Net_Disconnect()    -- clean shutdown

    Packet format (binary, length-prefixed):
        buf[0..1] = total packet length BE (including these 2 bytes)
        buf[2]    = packet type (SCCP_*)
        buf[3..]  = payload (ChaCha20-Poly1305 encrypted after handshake)

    Encrypted payload format:
        buf[0..11]  = nonce (12 bytes random)
        buf[12..]   = ChaCha20-Poly1305 ciphertext + 16 byte Poly1305 tag

    Call order per frame:
        SC_Net_ConnectPoll()   -- during connect phase only
        SC_Net_Poll()          -- drives recv state machine, queues packets
---------------------------------------------------------------------------*/


#ifdef __cplusplus
extern "C" {
#endif

    /* ── Packet types ─────────────────────────────────────────────────────────── */
#define SCCP_DH_INIT        0x01
#define SCCP_DH_RESPONSE    0x02
#define SCCP_REGISTER       0x03
#define SCCP_LOGIN          0x04
#define SCCP_AUTH_OK        0x05
#define SCCP_AUTH_FAIL      0x06
#define SCCP_ROOM_LIST      0x07
#define SCCP_JOIN_ROOM      0x08
#define SCCP_ROOM_INFO      0x09
#define SCCP_MESSAGE        0x0A
#define SCCP_MSG_RECV       0x0B
#define SCCP_HISTORY        0x0C
#define SCCP_ERROR          0x0D
#define SCCP_PING           0x0E
#define SCCP_PONG           0x0F
#define SCCP_DISCONNECT     0x10
#define SCCP_MSG_DELETE     0x19

/* ── Constants ────────────────────────────────────────────────────────────── */
#define SC_SERVER_PORT       8943
#define SC_MAX_PACKET        4096
#define SC_MAX_USERNAME      32
#define SC_MAX_PASSWORD      64
#define SC_MAX_MSGLEN        500
#define SC_MAX_ROOMS         32
#define SC_MAX_HISTORY       50
#define SC_TOKEN_LEN         128
#define SC_CHACHA_KEY_LEN    32
#define SC_CHACHA_NONCE_LEN  12
#define SC_POLY_TAG_LEN      16
#define SC_PACKET_QUEUE_SIZE 16

/* ── Connect states ───────────────────────────────────────────────────────── */
#define SC_STATE_IDLE        0
#define SC_STATE_LINK        1   /* waiting for network link / IP         */
#define SC_STATE_DNS         2   /* resolving hostname via XNetDnsLookup  */
#define SC_STATE_CONNECTING  3   /* TCP connect in progress               */
#define SC_STATE_HANDSHAKE   4   /* DH handshake                          */
#define SC_STATE_READY       5
#define SC_STATE_ERROR       6

/* Timeouts -- matching xb_net.cpp CS_TIMEOUT_* values */
#define SC_TIMEOUT_LINK      5000u
#define SC_TIMEOUT_DNS       5000u
#define SC_TIMEOUT_TCP       5000u

/* ── Room descriptor ──────────────────────────────────────────────────────── */
    typedef struct
    {
        unsigned char id;
        unsigned char type;                 /* 0=text  1=voice               */
        char          name[SC_MAX_USERNAME];
    } SC_Room;

    /* ── Message descriptor ───────────────────────────────────────────────────── */
    typedef struct
    {
        unsigned char room_id;
        char          username[SC_MAX_USERNAME];
        char          content[SC_MAX_MSGLEN];
        char          timestamp[8];
    } SC_Message;

    /* ── Auth result ──────────────────────────────────────────────────────────── */
    typedef struct
    {
        unsigned int user_id;
        char         token[SC_TOKEN_LEN + 1];
        char         username[SC_MAX_USERNAME];
    } SC_AuthResult;

    /* ── Room info result (join response + history) ───────────────────────────── */
    typedef struct
    {
        unsigned char room_id;
        unsigned char room_type;
        char          room_name[SC_MAX_USERNAME];
        SC_Message    history[SC_MAX_HISTORY];
        int           history_count;
    } SC_RoomInfo;

    /* ── Queued incoming packet ───────────────────────────────────────────────── */
    typedef struct
    {
        unsigned char type;
        unsigned char data[SC_MAX_PACKET];
        int           len;
    } SC_Packet;

    /* ── Connect API ──────────────────────────────────────────────────────────── */
    void SC_Net_Init(void);
    void SC_Net_Shutdown(void);
    int  SC_Net_ConnectBegin(const char* server_ip);
    int  SC_Net_ConnectPoll(void);   /* 0=working  1=done  -1=fail        */
    void SC_Net_Disconnect(void);
    int  SC_Net_IsReady(void);   /* 1 if session key derived          */
    int  SC_Net_GetState(void);
    void SC_Net_GetError(char* buf, int bufLen);

    /* ── Per-frame pump ───────────────────────────────────────────────────────── */
    void SC_Net_Poll(void);           /* recv, decrypt, enqueue -- every frame */

    /* ── Send API ─────────────────────────────────────────────────────────────── */
    int SC_Net_SendRegister(const char* username, const char* password);
    int SC_Net_SendLogin(const char* username, const char* password);
    int SC_Net_SendRoomList(void);
    int SC_Net_SendJoinRoom(unsigned char room_id);
    int SC_Net_SendMessage(unsigned char room_id, const char* content);
    int SC_Net_SendPing(void);

    /* ── Receive API ──────────────────────────────────────────────────────────── */
    /* Each returns 1 if a matching packet was waiting (and consumes it), else 0  */
    int SC_Net_RecvAuthResult(SC_AuthResult* pOut);
    int SC_Net_RecvRoomList(SC_Room* rooms, int* count);
    int SC_Net_RecvRoomInfo(SC_RoomInfo* pOut);
    int SC_Net_RecvMessage(SC_Message* pOut);
    int SC_Net_RecvError(char* buf, int bufLen);
    int SC_Net_RecvMessageEx(SC_Message* pOut, unsigned int* pMsgId);
    int SC_Net_RecvMsgDelete(unsigned char* pRoomId, unsigned int* pMsgId);

    /* ── Crypto (internal -- exposed for unit testing) ────────────────────────── */
    /*
       ChaCha20-Poly1305 as used by the server:
         encrypt: plain  -> nonce(12) | ciphertext | tag(16)
         decrypt: nonce(12) | ciphertext | tag(16)  -> plain
       key must be SC_CHACHA_KEY_LEN (32) bytes.
       nonce must be SC_CHACHA_NONCE_LEN (12) bytes.
    */
    int SC_Crypto_Encrypt(const unsigned char* key,
        const unsigned char* nonce,
        const unsigned char* plain, int plain_len,
        unsigned char* out, int* out_len);

    int SC_Crypto_Decrypt(const unsigned char* key,
        const unsigned char* nonce,
        const unsigned char* cipher, int cipher_len,
        unsigned char* out, int* out_len);

    /* Generate 12 random nonce bytes using XNetRandom */
    void SC_Crypto_RandNonce(unsigned char* nonce);

#ifdef __cplusplus
}
#endif

#endif /* SCENECHAT_NET_H */