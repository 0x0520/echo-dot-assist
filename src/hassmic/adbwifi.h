/* adb over Wi-Fi: closed on an installed Echo, opened for a while on request (scripts/device/lockdown.sh, adb_gate). */
#ifndef ADBWIFI_H
#define ADBWIFI_H
void adbwifi_start(void (*changed)(void));  /* watcher thread; changed(): adbwifi_open() changed, called without locks */
/* any thread: ask root's firewall watcher to open it (30 min) or close it.  from: the one IPv4 address to admit (dotted
 * quad), or NULL for what hassmic.conf allows (ADB_WIFI_FROM, else the whole network) */
void adbwifi_ask(int on, const char *from);
int  adbwifi_open(void);                    /* 1 while open, or asked for and not answered yet */
int  adbwifi_granted(void);                 /* the request has been taken and the port is open */
#endif
