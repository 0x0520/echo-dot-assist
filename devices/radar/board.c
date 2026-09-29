/* Echo 2nd gen 2017 (radar), Fire OS 6572.  Values checked on the running Echo (2026-09-28): getevent -il, thermal
 * zones, /proc/idme, led-resources.  Input is laid out like biscuit's, not donut's. */
#include "board.h"
#include <stddef.h>

const struct board board = {
    .model = "Echo 2 (radar)",
    .project = "hassmic.echo-2",
    .product = "Echo 2 (hassmic)",
    .default_name = "Echo",

    /* mtk-kpd: action (•) = KEY_HELP, mic mute = KEY_MUTE (and a volume key); "keys": volume ± (donut's event3 does
     * not exist here, so no button worked) */
    .keypad = "/dev/input/event1",
    .keypad2 = "/dev/input/event2",
    /* no gpio-privacy device: the mute is a key, toggled in software */
    .privacy_state = NULL,
    .privacy_input = NULL,
    .privacy_latch = 0,

    .bt_dev = "/dev/stpbt",                                         /* MediaTek combo, bluetooth:net_bt_stack */
    .bt_service = "btmanagerd",
    .bt_mac = "/proc/idme/bt_mac_addr",                             /* 12 hex digits */

    .wake_id = "alexa",
    .wake_manifest = "/system/local/models/keyword/en-US/ALEXA/pryon.manifest",   /* shipped in this build */
    .earcon_dir = "/system/local/share/earcon/base/",               /* same layout as donut */
    .thermal_type = "mtktscpu",                                     /* thermal_zone1 */
    .volume_steps = 30,                                             /* volume_step-01..30 in led-resources */
};
