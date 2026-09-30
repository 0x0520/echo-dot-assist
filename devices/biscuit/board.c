/* Echo Dot 2nd gen 2016 (biscuit), Fire OS 6574.1.  Values checked on the running Echo (probe 2026-09-28) except where
 * noted; device facts: docs/re-platform.md shows what each field is for (as found on donut). */
#include "board.h"
#include <stddef.h>

const struct board board = {
    .model = "Echo Dot 2 (biscuit)",
    .project = "hassmic.echo-dot-2",
    .product = "Echo Dot 2 (hassmic)",
    .default_name = "Echo Dot",

    /* mtk-kpd: action (•) = KEY_HELP, mic mute = KEY_MUTE; gpio-keys: volume ± */
    .keypad = "/dev/input/event1",
    .keypad2 = "/dev/input/event2",
    /* Mute latch kept by the keypad driver, as on radar (same SoC, same line in init.mt8163_amazon.rc).  Path from the
     * firmware's init rc, not yet read on a running biscuit: if it is missing, hassmic counts key presses instead. */
    .privacy_state = "/sys/devices/soc/10010000.keypad/amz_privacy/state",
    .privacy_input = NULL,
    .privacy_latch = 1,

    .bt_dev = "/dev/stpbt",                                         /* bluetooth:net_bt_stack on the Echo */
    .bt_service = "btmanagerd",
    .bt_mac = "/proc/idme/bt_mac_addr",                             /* 12 hex digits, read on the Echo */

    .wake_id = "alexa",
    .wake_manifest = "/system/local/models/keyword/en-US/ALEXA/pryon.manifest",   /* confirmed on the Echo */
    .earcon_dir = "/system/local/share/earcon/base/",
    .thermal_type = "mtktscpu",                                     /* among the Echo's thermal zones */
    .volume_steps = 30,                                             /* volume_step-01..30 in led-resources */
};
