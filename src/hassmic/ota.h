/* Push updates: receive a signed bundle over TCP and hand it to the root-side installer (scripts/system/main.sh). */
#ifndef OTA_H
#define OTA_H
int ota_start(int port);        /* listener thread; does nothing useful without /system/hassmic/update.pub.  Also
                                   opens adb over Wi-Fi, and makes an approved update the factory copy, for whoever
                                   holds the update key (ota.c) */
#include <stddef.h>
#include <stdint.h>
/* A bundle hassmic downloaded itself (update.c), signed with the release key: checked against that key, handed to root's
 * installer like a push.  RES gets the installer's answer ("OK <version>": hassmic is restarted on it; "FAILED <why>").
 * 0 on OK.  Blocks for up to a minute. */
int ota_handoff(const uint8_t *bundle, size_t len, const uint8_t sig[64], char *res, size_t cap);
#endif
