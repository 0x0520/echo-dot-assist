/* Wi-Fi setup over Bluetooth: Improv Wi-Fi (https://www.improv-wifi.com/ble/), as ESPHome devices offer it.  Home
 * Assistant's Improv integration or the Improv app hands the Echo an SSID and passphrase; root joins the network
 * (scripts/system/main.sh, wifi_watch).  Only for a while: when the Echo has had no Wi-Fi address for 2 min after a
 * boot or a lost link, or after the action button was held 5 s; and only once the action button has been pressed. */
#ifndef IMPROV_H
#define IMPROV_H
#include <stddef.h>
#include <stdint.h>

struct improv_handler {
    void (*window)(int open);           /* the setup window opened or closed (improv thread, no lock held) */
    void (*identify)(void);             /* a client asked the Echo to make itself known (controller thread) */
};

/* main(), once the radio is being taken (a2dp_start): the device info it reports, the thread */
void improv_start(const struct improv_handler *h, const char *name, const char *version);
int  improv_button(void);               /* action button pressed: 1 if Improv took it (authorization) */
void improv_hold(void);                 /* action button held 5 s: open the window */
int  improv_enable(int set);            /* the "Wi-Fi setup over Bluetooth" switch: 0/1, -1 reads.  Settings field 17 */
int  improv_open(void);                 /* the window is open */

/* ---------------------------------------------------------------- protocol (tests/unit/improv_test.c) */
enum { IMPROV_AUTH_REQUIRED = 1, IMPROV_AUTHORIZED, IMPROV_PROVISIONING, IMPROV_PROVISIONED };
enum { IMPROV_E_NONE = 0, IMPROV_E_INVALID_RPC, IMPROV_E_UNKNOWN_RPC, IMPROV_E_UNABLE_TO_CONNECT, IMPROV_E_NOT_AUTHORIZED,
       IMPROV_E_UNKNOWN = 0xff };
enum { IMPROV_WIFI = 1, IMPROV_IDENTIFY, IMPROV_DEVICE_INFO };
enum { IMPROV_CAP_IDENTIFY = 1, IMPROV_CAP_DEVICE_INFO = 2 };

unsigned improv_checksum(const uint8_t *p, size_t n);
/* An RPC command: its id, data at *data (*len bytes); -1 if the frame or its checksum is wrong */
int    improv_rpc_parse(const uint8_t *p, size_t n, const uint8_t **data, size_t *len);
/* Send Wi-Fi Settings: 0, -1 malformed, -2 SSID or passphrase that no WPA network can have.  ssid: 1..32 bytes (any);
 * psk: "" (open network), 8..63 printable ASCII, or 64 hex digits */
int    improv_wifi_parse(const uint8_t *d, size_t n, uint8_t ssid[32], size_t *ssid_len, char psk[65]);
size_t improv_rpc_result(unsigned cmd, const char *const *strs, int n, uint8_t *out, size_t cap);
size_t improv_adv_data(unsigned state, unsigned caps, uint8_t out[31]);
size_t improv_scan_rsp(const char *name, uint8_t out[31]);

/* ---------------------------------------------------------------- the machinery, driven by the improv thread every
 * 500 ms with the time and whether wlan0 has an address; tests drive it with a clock of their own.  The other entry
 * points take the time of the last tick. */
void   improv_init(const struct improv_handler *h, const char *name, const char *version);   /* improv_start without
                                                                                            the thread and the radio */
void   improv_tick(long long now_ms, int wifi_up);
int    improv_state(void);
int    improv_error(void);
/* the ble_peripheral callbacks (ble.h), controller thread */
int    improv_advertise(uint8_t *adv, size_t *adv_len, uint8_t *rsp, size_t *rsp_len);
void   improv_connected(uint64_t addr, int on);
size_t improv_att(const uint8_t *req, size_t n, uint8_t *rsp, size_t cap);
size_t improv_notify(uint8_t *pdu, size_t cap);
#endif
