/* Private to the BR/EDR side of hassmic's Bluetooth (the speaker both ways, a2dp.h): what its modules share.
 *
 *   bt_link.c       link keys, pairing window, GAP and the BR/EDR events, links, ACL, L2CAP signalling and channels;
 *                   the hooks ble.c calls (hci.h) and a2dp_start
 *   sdp.c           the SDP server
 *   a2dp_sink.c     a phone playing to us: AVDTP sink endpoints, jitter buffer and player thread
 *   avrcp.c         AVRCP both roles on the phones' links, and the Echo's volume as Bluetooth sees it
 *   a2dp_source.c   the Echo playing to a Bluetooth speaker: search, paging, AVDTP initiator, its absolute volume
 *
 * Threads and locks.  Everything declared here runs on the controller thread (ble.c's), which calls into these modules
 * only through hci.h, one event, ACL packet or upkeep at a time; none of it takes a lock, and hci_cmd() is only ever
 * sent from upkeep or setup, never while an event is handled (ble.c refuses it there).  The other threads are the
 * a2dp.h API (any thread: it leaves requests in atomics and wakes the controller thread with hci_poke), a2dp_sink.c's
 * player (the ring under its r_lock) and avrcp.c's volume thread (core_lock).  State they share with the controller
 * thread is atomic or under a lock named where it is declared.
 */
#ifndef BT_INT_H
#define BT_INT_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "acl.h"

#define MAX_LINKS 3                     /* a phone playing to us, the speaker we play to, one more */
#define MAX_CHANS 6                     /* per link: SDP, AVDTP signalling + media, and room for a retry */

enum { OP_INQUIRY = 0x0401, OP_INQUIRY_CANCEL, OP_CREATE_CONN = 0x0405, OP_DISCONNECT = 0x0406, OP_ACCEPT = 0x0409, OP_REJECT, OP_LINK_KEY_REPLY, OP_LINK_KEY_NEG, OP_PIN_REPLY, OP_PIN_NEG,
       OP_AUTH = 0x0411, OP_ENCRYPT = 0x0413, OP_REMOTE_NAME = 0x0419, OP_IO_CAP_REPLY = 0x042b, OP_CONFIRM_REPLY, OP_CONFIRM_NEG, OP_IO_CAP_NEG = 0x0434,
       OP_LINK_POLICY = 0x080f, OP_LOCAL_NAME = 0x0c13, OP_SCAN_ENABLE = 0x0c1a, OP_CLASS = 0x0c24, OP_INQUIRY_MODE = 0x0c45,
       OP_EIR = 0x0c52, OP_SSP_MODE = 0x0c56, OP_READ_BUFFER = 0x1005 };
enum { PSM_SDP = 0x0001, PSM_AVCTP = 0x0017, PSM_AVDTP = 0x0019 };

/* AVDTP: signals, error codes (a2dp_sink.c and a2dp_source.c) */
enum { AV_DISCOVER = 1, AV_GET_CAP, AV_SET_CONF, AV_GET_CONF, AV_RECONF, AV_OPEN, AV_START, AV_CLOSE, AV_SUSPEND, AV_ABORT,
       AV_SECURITY, AV_GET_ALL_CAP, AV_DELAY };
enum { E_BAD_ACP_SEID = 0x12, E_SEP_IN_USE, E_BAD_SERV_CATEGORY = 0x17, E_NOT_SUPPORTED = 0x19, E_UNSUPPORTED_CONF = 0x29,
       E_BAD_STATE = 0x31 };

/* AV/C and AVRCP (avrcp.c and a2dp_source.c) */
enum { AVC_CONTROL = 0x00, AVC_STATUS = 0x01, AVC_NOTIFY = 0x03, AVC_NOT_IMPLEMENTED = 0x08, AVC_ACCEPTED, AVC_REJECTED, AVC_STABLE = 0x0c,
       AVC_CHANGED, AVC_INTERIM = 0x0f };
enum { AVC_VENDOR = 0x00, AVC_UNIT_INFO = 0x30, AVC_SUBUNIT_INFO, AVC_PASS_THROUGH = 0x7c };
enum { PDU_GET_CAPS = 0x10, PDU_REGISTER = 0x31, PDU_SET_VOLUME = 0x50, EVENT_VOLUME = 0x0d, KEY_PLAY = 0x44, KEY_PAUSE = 0x46 };
#define PANEL 0x48                                      /* subunit type panel (9), ID 0 */

struct chan { int used, psm, lcid, rcid, rmtu, cfg_in, cfg_out, pending, pend_id, out_id; };   /* out_id: ours, awaiting the answer */
struct link {
    int used, handle, enc, auth_sent, dropping; uint64_t addr;
    struct acl_tx tx;                           /* tx.queued: our ACL fragments not yet sent */
    long long auth_at, pend_until;              /* AVDTP waits for encryption: when we authenticate ourselves, give up */
    unsigned char rx[4 + 2048]; struct l2cap_rx rxs;
    struct chan ch[MAX_CHANS]; int next_id, av_sig;     /* av_sig: our CID of the AVDTP signalling channel, 0 = none */
    long long avctp_at;                         /* when we open AVRCP ourselves if the device has not (0: done / not due) */
    int avctp, av_label, vol_label, vol_pct;    /* AVRCP: our CID, 0 = none; next transaction label of ours; label of the
                                                   device's volume notification (-1: none registered), the volume it knows */
    char name[80]; int named, told;             /* the device's name (Remote Name Request), asked yet; connect announced */
};

static inline long long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static inline unsigned u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static inline void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static inline uint64_t addr_of(const unsigned char *p) { uint64_t a = 0; for (int i = 5; i >= 0; i--) a = a << 8 | p[i]; return a; }

/* ---------------------------------------------------------------- bt_link.c */
void bt_notify(void);                                   /* a2dp_pairing() or the speaker's status changed: tell a2dp_start's
                                                           callback */
void later(unsigned op, const void *p, unsigned n);     /* an HCI command for the next upkeep */
struct link *link_next(struct link *prev);              /* the links in use: link_next(NULL) first, NULL after the last */
struct chan *chan_by(struct link *l, int lcid);
void l2_send(struct link *l, unsigned cid, const void *pdu, size_t n);
int  chan_open(struct link *l, int psm);                /* a channel of ours: its CID, 0 = no room */
void key_forget(uint64_t a);

/* ---------------------------------------------------------------- sdp.c */
void sdp_rx(struct link *l, struct chan *c, const unsigned char *p, size_t n);

/* ---------------------------------------------------------------- a2dp_sink.c */
void sink_start(void);                                  /* endpoints for the usable codecs, player thread */
struct link *sink_link(void);                           /* the link with the configured stream, NULL: none */
int  sink_streaming(void);                              /* AVDTP says the source plays (any thread) */
void av_send(struct link *l, const void *p, size_t n);  /* on the link's AVDTP signalling channel */
void av_reply(struct link *l, unsigned label, unsigned sig, int accept, const unsigned char *d, size_t n);
void av_rx(struct link *l, const unsigned char *p, size_t n);          /* signalling from a source */
void sink_media_opened(struct link *l, struct chan *c);
void sink_closed(struct link *l);                       /* its AVDTP signalling or the whole link is gone: so is its stream */
void sink_media_closed(struct link *l, int lcid);
void sink_media_rx(struct link *l, int cid, const unsigned char *p, size_t n);

/* ---------------------------------------------------------------- avrcp.c */
void volume_init(void);                                 /* the Echo's volume as it is at start */
void avrcp_start(void);                                 /* the volume thread */
int  volume_now(void);                                  /* 0..100, kept up to date by the core */
void volume_note(int pct);                              /* a device set it: what volume_now() says from now on */
void volume_set_later(int pct);                         /* set the Echo's volume, off the controller thread */
void avctp_send(struct link *l, unsigned label, int response, const unsigned char *avc, size_t n);
void avrcp_rx(struct link *l, const unsigned char *p, size_t n);
void avrcp_opened(struct link *l, struct chan *c);      /* the AVCTP channel is up */
void avrcp_closed(struct link *l);
int  avrcp_present(void);                               /* some link has AVRCP (any thread) */
void avrcp_press(int key);                              /* KEY_PLAY / KEY_PAUSE to the device (any thread) */
void avrcp_upkeep(void);

/* ---------------------------------------------------------------- a2dp_source.c */
void out_start(void);                                   /* the speaker remembered, btout's side */
int  is_speaker(const struct link *l);                  /* the one we play to */
int  out_may_pair(uint64_t a);
int  out_upkeep(void);                                  /* -1: controller gone */
void out_rx(struct link *l, const unsigned char *p, size_t n);         /* AVDTP signalling on the speaker's link */
void out_avrcp_answer(unsigned code, unsigned pdu, const unsigned char *par, size_t n);
void out_found(const unsigned char *r, size_t n, int eir);              /* an inquiry response */
void out_inquiry_done(void);
int  out_page_complete(uint64_t a, unsigned status);   /* Connection Complete: 0 not our page, 1 our page succeeded,
                                                           -1 our page failed (handled) */
void out_link_up(struct link *l, int paged);
void out_link_lost(struct link *l);
void out_named(struct link *l);                         /* the link's name is in */
int  out_paired(uint64_t a);                            /* a new link key: 1 = the speaker's */
void out_sig_opened(struct link *l, struct chan *c);
int  out_media_opened(struct link *l, struct chan *c);  /* 1: the speaker's link (handled) */
void out_avctp_closed(struct link *l);
void out_sig_closed(struct link *l);
int  out_media_closed(struct link *l, int lcid);        /* 1: the speaker's media channel (handled) */
int  out_busy(void);                                    /* timers running: upkeep wanted */
void out_lost(void);                                    /* controller gone */
int  out_streaming(void);                               /* any thread */
#endif
