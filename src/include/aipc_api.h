/*
 * Server side of Amazon's AIPC (libace_aipc.so), recovered from NS65741 by disassembly (docs/re-a2dp-source.md),
 * and the requests the mixer makes of the Bluetooth service on it.  libace_aipc is byte-identical on donut and
 * biscuit.  Loaded with dlopen: the PC builds have no such library.
 *
 * A service is a SOCK_SEQPACKET socket /dev/aipc/<uuid>/ss.  aceAipc_start removes a leftover directory first, which
 * fails for one owned by another user (btmanagerd's: root removes it).  The client side refuses uid 0, the server
 * side too.  Requests arrive on a worker thread of the library; the handler writes the answer into task->data and the
 * library sends it back (synchronous calls), whatever the handler returns.
 */
#ifndef AIPC_API_H
#define AIPC_API_H
#include <stdint.h>

struct aipc_task {
    uint16_t client_id, remote_uuid;
    uint32_t type;                      /* 0 call, 3 event (0xfffb client came, 0xfffc client went) */
    uint16_t function_id, event_id;
    uint32_t *len;                      /* in and out */
    int32_t *status;                    /* sent with the answer; 0 unless the handler sets it */
    void *data;                         /* request in, answer out, in place */
    uint32_t cb_type;
    uint16_t cb_id, _p;                 /* 0xff01: synchronous */
    uint32_t sync;
    uint32_t _24, _28, _2c;
    int32_t pid, uid, gid;              /* the client's (SO_PEERCRED) */
};

struct aipc_server_cfg {                /* 0x4a0 bytes, zeroed apart from what btmanagerd sets */
    uint16_t uuid, _p0;
    uint32_t _r04;
    int (*handler)(struct aipc_task *);
    uint32_t max_payload;               /* at most 0x4000; btmanagerd 0x2800 */
    uint16_t r10, _p1;                  /* btmanagerd 10 */
    uint32_t r14;                       /* btmanagerd 1 */
    uint32_t _r18, _r1c;
    uint32_t thread_option;             /* 0: aceAipc_start joins the server thread, i.e. never returns (btmanagerd calls
                                           it on a thread of its own); 1: returns once the service is up */
    uint32_t _r24, _r28;
    char name[100];                     /* the library writes "%u" of the uuid */
    uint8_t _r90[0x400];
    uint32_t through_proxy, single_thread, thread_pool;   /* 0xdeadbeef = on; all off like btmanagerd */
    uint8_t pool_threads, _p2[3];
};

typedef int (*aceAipc_start_fn)(int *handle, const struct aipc_server_cfg *cfg);   /* 0, -7 uid 0, -11 bad config */
typedef int (*aceAipc_stop_fn)(int *handle);                                        /* removes the socket and its dir */

/* The Bluetooth service, uuid 0 (libace_bt's client).  Packed, little-endian, offsets into task->data. */
#define AIPC_BT_UUID            0
#define AIPC_BT_SESSION_OPEN    0x00    /* len 0x1b; answer: u32 session at 1, s32 status at 7 (must be 0) */
#define AIPC_BT_SESSION_CLOSE   0x01    /* len 9: u32 session at 1; answer: s32 status at 5 */
#define AIPC_BT_GET_NAME        0x13    /* len 0x103: bdaddr at 4; answer: s32 status at 0, name[249] at 0x0a */
#define AIPC_BT_A2DP_SRC_DISCONNECT 0xca  /* len 12: bdaddr at 0; answer: s32 status at 8 */
#endif
