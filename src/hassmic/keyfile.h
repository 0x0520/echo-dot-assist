/* Files of secret keys in hassmic's state directory ($HASSMIC_STATE, else /data/local/hassmic/state): the BR/EDR link
 * keys (a2dp.c) and the LE bonds (ble.c).  Written whole to a temporary file created 0600 and renamed over the old
 * one, so a crash leaves the old file or the new one, never half of one, and the keys are never readable by others. */
#ifndef KEYFILE_H
#define KEYFILE_H
#include <stdio.h>

struct keyfile { FILE *f; const char *tag; char path[256], tmp[300]; };

FILE *keyfile_read(const char *name);                                       /* NULL: none (yet) */
/* opens the temporary file to write into: k->f, NULL (logged with tag) if it cannot */
FILE *keyfile_write(struct keyfile *k, const char *name, const char *tag);
void keyfile_commit(struct keyfile *k);                                    /* closes, replaces the old file; logged */
#endif
