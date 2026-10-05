/* The controller thread (ble.c) shared with the Bluetooth speaker (bt_link.c and the modules of bt_int.h), with the LE
 * peripheral (ble_periph.c) and with acl.c.  Everything here runs on that thread. */
#ifndef HCI_H
#define HCI_H
#include <stddef.h>
struct acl_tx;

/* ble.c */
int  hci_cmd(unsigned op, const void *par, unsigned n);    /* waits for Command Complete / Status: HCI status, -1 = gone.
                                                               Never from inside event handling (it pumps events itself):
                                                               ble.c logs such a call as a bug and returns -1 */
const unsigned char *hci_ret(void);                         /* the last Command Complete's parameters after the status */
int  hci_write(const void *h4, size_t n);                   /* one H4 packet as it is */
void hci_poke(void);                                        /* any thread: wake the controller thread for upkeep */
/* When the controller has no LE buffers of its own (LE Read Buffer Size says 0), LE and BR/EDR share its ACL buffers:
 * then this is the one count of free ones both sides take from (NULL: separate buffers, a2dp counts its own).  Two
 * counts of the same buffers would let the controller overflow.  Valid from a2dp_setup on. */
int *hci_acl_pool(void);
/* LE's ACL queue, for the peripheral's link: one L2CAP frame (as l2cap_send), and the link gone (as acl_forget) */
void le_send(int handle, struct acl_tx *tx, unsigned cid, const void *pdu, size_t n);
void le_forget(int handle, struct acl_tx *tx);

/* bt_link.c, called by ble.c */
int  a2dp_setup(void);                                      /* after the reset; -1 = controller gone */
int  a2dp_event(const unsigned char *p, size_t n);          /* event code, length, parameters.  1 = a BR/EDR one, handled */
int  a2dp_acl(const unsigned char *p, size_t n);            /* ACL packet after the H4 byte.  1 = on a BR/EDR link */
int  a2dp_completed(unsigned handle, unsigned n);           /* Number Of Completed Packets.  1 = a BR/EDR link's */
void a2dp_acl_flush(void);                                  /* shared pool: LE packets completed, a2dp's queue may go on */
int  a2dp_upkeep(void);                                     /* HCI commands the events asked for, timeouts.  -1 = gone */
int  a2dp_busy(void);                                       /* timers running: upkeep wanted even without events */
void a2dp_lost(void);                                       /* controller gone: every link with it */
int  a2dp_streaming(void);                                  /* audio flows: LE scanning pauses, the radio is busy enough */

/* ble_periph.c, called by ble.c */
int  periph_event(const unsigned char *p, size_t n);        /* event code, length, parameters.  1 = the peripheral link's */
int  periph_acl(const unsigned char *p, size_t n);          /* ACL packet after the H4 byte.  1 = on the peripheral link */
struct acl_tx *periph_tx(int handle);                       /* the peripheral link's ACL counts, NULL: not its handle */
int  periph_upkeep(int central_busy);                       /* advertising, the link's commands, notifications.  central_busy:
                                                               ble.c sets up a connection of its own.  -1 = gone */
int  periph_scan_pause(void);                               /* advertising wants the scan off (the controller does one) */
int  periph_busy(void);                                     /* upkeep wanted even without events */
void periph_lost(void);                                     /* controller gone: the link, advertising, what it knew */
#endif
