/*
 * hassmic_rcpi4m: the receive level of every frame from the access point, for Wi-Fi motion (src/hassmic/wifimotion.c)
 * on the Echo Dot 3 (donut: MT7668, MediaTek's gen4m driver wlan_mt76x8_sdio.ko, a module of its own; kernel 4.4.22,
 * arm64).  hassmic_rcpi.c's counterpart for that driver.  The driver's own RX_STAT answer is the firmware's figure for
 * the last frame from anyone on the channel, every station and neighbouring network included, and one per ioctl; this
 * counts only what reaches the driver's data path, which in station mode is the access point's, frame by frame.
 *
 *   /proc/hassmic_rcpi4m   "RCPI RX0 = 112\nAGE = 37\nFRAMES = 5120\nKIND = 1000b\n" (as hassmic_rcpi.c: RX_STAT's
 *                          words, AGE in ms since that frame, FRAMES since load; KIND: how that frame was sent, RX
 *                          vector word 0 bits 0-6 rate/MCS, 12-14 mode, 15-16 bandwidth).  In RAM: nothing touches the
 *                          flash.
 * KIND because the access point sends each rate at its own power (wifimotion.c: broadcasts at 6 Mbit/s 7-9 dB over its
 * VHT unicast frames, MCS 9 2 dB under MCS 8), so levels compare only within one rate.  Seen on donut (2026-10-01):
 * 0x1000b broadcasts (legacy OFDM, 20 MHz, 6 Mbit/s; WTBL entry 4), 0x14008 / 0x14009 the access point's unicast
 * frames (VHT, 80 MHz, MCS 8 / 9; entry 5).
 *
 * Where: nicRxProcessDataPacket(ADAPTER_T *, SW_RFB_T *) calls nicRxFillRFB(ADAPTER_T *, SW_RFB_T *) once, first
 * thing (+0x50), which parses the RX descriptor's groups into the SW_RFB_T: ucGroupVLD at +32 (bit 2: group 3, the RX
 * vector), prRxStatusGroup3 at +64.  RCPI0 is byte 0 of RX vector word 3, as the driver's own nicRxGetRcpiValueFromRxv
 * reads it (half dB, dBm = RCPI / 2 - 110; one receive chain, RCPI1 is 255).  The same function goes on to copy those
 * words into the access point's STA_RECORD_T for every such frame.
 *
 * How: the kernel has no kprobes.  That one `bl nicRxFillRFB` becomes `bl hm_fill`, which calls nicRxFillRFB itself and
 * then reads the levels; no trampoline, no displaced instruction.  The call is found by its target, not its offset,
 * and patched only if exactly one call to it is in the first scan_max instructions.  A bl for a bl is one of the
 * instructions arm64 lets be patched while other CPUs run it (aarch64_insn_patch_text, which also writes through the
 * fixmap where module text is read-only).  hm_rx() reads through probe_kernel_read: a field that is not where expected
 * gives no reading, not an oops.  No module_exit, as hassmic_rcpi.c: a thread may sit in hm_fill.  Layout from the
 * driver in donut's NS65741 image (PLAN.md); module parameters for another build (device.conf KMOD_ARGS).  Built against
 * kernel.org 4.4.22 with devices/donut/kconfig and AOSP's aarch64-linux-android-4.9 (Makefile).
 */
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>

static char *hook_sym = "nicRxProcessDataPacket";
static char *fill_sym = "nicRxFillRFB";
static uint scan_max = 64;          /* instructions of hook_sym searched for the call (it is the 21st) */
static uint rfb_vld = 32;           /* SW_RFB_T.ucGroupVLD: nicRxFillRFB, from RX descriptor DW0 bits 25-28 */
static uint vld_grp3 = 2;           /* its bit for group 3 */
static uint rfb_grp3 = 64;          /* SW_RFB_T.prRxStatusGroup3 */
static uint grp3_rcpi = 12;         /* RCPI0 in group 3: RX vector word 3, byte 0 (nicRxGetRcpiValueFromRxv) */
static uint rate_mask = 0x1f07f;    /* rate, mode, bandwidth in RX vector word 0 */
module_param(hook_sym, charp, 0444);
module_param(fill_sym, charp, 0444);
module_param(scan_max, uint, 0444);
module_param(rfb_vld, uint, 0444);
module_param(vld_grp3, uint, 0444);
module_param(rfb_grp3, uint, 0444);
module_param(grp3_rcpi, uint, 0444);
module_param(rate_mask, uint, 0444);

static atomic_t last = ATOMIC_INIT(-1), frames = ATOMIC_INIT(0);    /* last: RCPI | kind << 8, one frame's pair */
static unsigned long last_jiffies;
static u8 (*fill)(void *adapter, void *rfb);    /* BOOLEAN nicRxFillRFB(): the caller tests w0 & 0xff */

static void hm_rx(void *rfb)
{
    u8 vld, *grp3, rcpi; u32 rxv0;
    if (probe_kernel_read(&vld, (u8 *)rfb + rfb_vld, 1) || !(vld & BIT(vld_grp3))) return;
    if (probe_kernel_read(&grp3, (u8 *)rfb + rfb_grp3, sizeof grp3) || probe_kernel_read(&rcpi, grp3 + grp3_rcpi, 1)
        || probe_kernel_read(&rxv0, grp3, 4)) return;
    if (!rcpi || rcpi == 255) return;
    atomic_set(&last, rcpi | (rxv0 & rate_mask & 0x7fffff) << 8); ACCESS_ONCE(last_jiffies) = jiffies; atomic_inc(&frames);
}

static u8 hm_fill(void *adapter, void *rfb)
{
    u8 ok = fill(adapter, rfb);
    if (ok) hm_rx(rfb);
    return ok;
}

static int show(struct seq_file *m, void *v)
{
    int r = atomic_read(&last);
    if (r < 0) { seq_puts(m, "ERROR = no frame yet\n"); return 0; }
    seq_printf(m, "RCPI RX0 = %d\nAGE = %u\nFRAMES = %d\nKIND = %x\n", r & 0xff, jiffies_to_msecs(jiffies - ACCESS_ONCE(last_jiffies)),
               atomic_read(&frames), r >> 8);
    return 0;
}

static int open_fn(struct inode *inode, struct file *file) { return single_open(file, show, NULL); }
static const struct file_operations fops = { .owner = THIS_MODULE, .open = open_fn, .read = seq_read, .llseek = seq_lseek, .release = single_release };

static int is_bl_to(u32 insn, unsigned long at, unsigned long to)
{
    long off = sign_extend64((insn & 0x03ffffff) << 2, 27);     /* imm26 words */
    return (insn & 0xfc000000) == 0x94000000 && at + off == to;
}

static int __init init_fn(void)
{
    int (*patch_text)(void *addrs[], u32 insns[], int cnt);
    unsigned long caller = kallsyms_lookup_name(hook_sym), target = kallsyms_lookup_name(fill_sym), site = 0;
    void *addr; u32 insn; long off; uint i, n = 0;
    patch_text = (void *)kallsyms_lookup_name("aarch64_insn_patch_text");
    if (!caller || !target || !patch_text) {
        pr_err("hassmic_rcpi4m: %s, %s or aarch64_insn_patch_text not in this kernel (Wi-Fi driver loaded?)\n", hook_sym, fill_sym);
        return -ENOENT;
    }
    for (i = 0; i < scan_max; i++) {
        if (probe_kernel_read(&insn, (void *)(caller + 4 * i), 4)) break;
        if (is_bl_to(insn, caller + 4 * i, target)) { site = caller + 4 * i; n++; }
    }
    if (n != 1) { pr_err("hassmic_rcpi4m: %u calls to %s in %s's first %u instructions, not 1: not patched\n", n, fill_sym, hook_sym, scan_max); return -EINVAL; }
    off = (long)hm_fill - (long)site;
    if (off < -SZ_128M || off >= SZ_128M) { pr_err("hassmic_rcpi4m: hook out of branch range\n"); return -ERANGE; }
    if (!proc_create("hassmic_rcpi4m", 0444, NULL, &fops)) return -ENOMEM;
    fill = (void *)target;
    addr = (void *)site;
    insn = 0x94000000 | ((off >> 2) & 0x03ffffff);              /* bl hm_fill */
    if (patch_text(&addr, &insn, 1)) { remove_proc_entry("hassmic_rcpi4m", NULL); pr_err("hassmic_rcpi4m: patching failed\n"); return -EIO; }
    pr_info("hassmic_rcpi4m: %s+%#lx (call to %s) hooked, /proc/hassmic_rcpi4m\n", hook_sym, site - caller, fill_sym);
    return 0;
}

module_init(init_fn);
MODULE_LICENSE("GPL");      /* kallsyms_lookup_name is GPL-only */
MODULE_DESCRIPTION("RCPI of the frames from the access point, from MediaTek's gen4m Wi-Fi driver, for hassmic's Wi-Fi motion");
