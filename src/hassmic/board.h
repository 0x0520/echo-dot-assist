/* What differs between Echo models, as far as the daemon is concerned: identity, device nodes, stock file locations.
 * One definition per model in devices/<codename>/board.c, picked at link time by DEVICE in the Makefile (the PC builds link
 * the same one, so the protocol tests see the identity of the model they stand in for).  Everything else in src/hassmic is
 * meant to be the same on every model; when a port needs more than a different value here, add a field or a backend
 * (audio.h, wake.h) rather than an #ifdef. */
#ifndef BOARD_H
#define BOARD_H

struct board {
    /* identity towards Home Assistant and Music Assistant */
    const char *model;              /* ESPHome device info "model", Wyoming description */
    const char *project;            /* ESPHome "project_name" */
    const char *product;            /* Sendspin "product_name" */
    const char *default_name;       /* friendly name when -n is not given */

    /* inputs */
    const char *keypad;             /* input device with action and volume keys (-b overrides) */
    const char *privacy_state;      /* sysfs file, '1' = mics muted by the hardware latch */
    const char *privacy_input;      /* input device that reports changes of that latch; NULL if the keypad does */

    /* Bluetooth: raw HCI (H4) character device, the init service that owns it in stock, where the address is stored */
    const char *bt_dev;
    const char *bt_service;
    const char *bt_mac;

    /* stock assets */
    const char *wake_id;            /* id of the firmware's own wake word model */
    const char *wake_manifest;
    const char *earcon_dir;         /* with trailing slash */
    const char *thermal_type;       /* thermal zone reported as SoC temperature */
    int volume_steps;               /* volume_step-NN animations of ledcontroller */
};

extern const struct board board;
#endif
