/*
 * hassmic_rcpi: the receive level of every frame from the access point, for Wi-Fi motion (src/hassmic/wifimotion.c) on
 * Echos whose Wi-Fi driver does not report it (biscuit, radar: MediaTek's gen2 driver for the MT8163 connsys, built into
 * the kernel).  Its own interfaces only hand out the firmware's smoothed RSSI in whole dB (/proc/net/wireless,
 * signal_poll), and the access point's beacons, whose level the driver does keep (BSS_DESC_T), stop reaching it once it
 * is connected: the firmware filters them (entry 4 h old on biscuit).  Every received frame carries its RCPI in byte 9
 * of its RX header (half dB, dBm = RCPI / 2 - 110), which the driver reads for beacons only; in station mode every
 * frame comes from the access point.
 *
 *   /proc/hassmic_rcpi     "RCPI RX0 = 112\nAGE = 37\nFRAMES = 5120\nKIND = 0\n" (RX_STAT's words, so wifimotion.c has
 *                          one parser; AGE: ms since that frame; FRAMES: counted since load; KIND 1: sent to a group
 *                          address, 0: to us).  In RAM: nothing touches the flash.
 * KIND because an access point sends its broadcasts at a basic rate and more power than its unicast frames (donut on
 * 5 GHz, 2026-10-01: 7-9 dB more, frames of both interleaved; wifimotion.c), so levels compare only within a kind.  The
 * gen2 RX header carries no rate (bytes 5-7: reorder flags, sequence number, TID), so unicast frames at different rates
 * stay one kind.  Data frames reach the driver as 802.3 (nicRxProcessDataPacket reads the ethertype at +12), behind the
 * 12-byte header and (byte 4 & 3) bytes of padding: the destination address comes first, bit 0 = group.
 *
 * How: the kernel has no kprobes, and its text is writable (no DEBUG_RODATA).  The first instruction of
 * nicRxProcessDataPacket(ADAPTER_T *, SW_RFB_T *) becomes a branch to hm_tramp, which calls hm_rx(), runs that
 * instruction (push, position-independent) and jumps back.  It is only patched if it is exactly the instruction this was
 * worked out against, under stop_machine.  hm_rx() reads through probe_kernel_read: a header that is not where expected
 * gives no reading, not an oops.  No module_exit: unpatching while a preempted thread sits in hm_tramp would be worse
 * than keeping it until the next reboot.  Layout from biscuit's kernel (3.18.19, PLAN.md); module parameters for
 * another (device.conf KMOD_ARGS).  Built against kernel.org 3.18.19 with the Echo's /proc/config.gz and AOSP's
 * arm-eabi-4.8 (Makefile target kmod).
 */
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/stop_machine.h>
#include <linux/uaccess.h>
#include <asm/cacheflush.h>

static char *hook_sym = "nicRxProcessDataPacket";
static uint hook_insn = 0xe92d43f8;  /* push {r3, r4, r5, r6, r7, r8, r9, lr}: what the hook replaces and runs itself */
static uint rfb_hdr = 16;            /* SW_RFB_T.prHifRxHdr: nicRxProcessDataPacket's first load, scanAddToBssDesc */
static uint hdr_rcpi = 9;            /* RCPI in the RX header: scanAddToBssDesc copies it into BSS_DESC_T.ucRCPI */
static uint hdr_len = 12;            /* the RX header in front of the frame (nicRxFillRFB: + byte 4 & 3) */
module_param(hook_sym, charp, 0444);
module_param(hook_insn, uint, 0444);
module_param(rfb_hdr, uint, 0444);
module_param(hdr_rcpi, uint, 0444);
module_param(hdr_len, uint, 0444);

static atomic_t last = ATOMIC_INIT(-1), frames = ATOMIC_INIT(0);    /* last: RCPI | kind << 8, one frame's pair */
static unsigned long last_jiffies;
static u32 *hook_at;
unsigned long hm_ret;                /* hook_at + 4, for hm_tramp */

void hm_rx(void *adapter, void *rfb)
{
    u8 *hdr, rcpi, pad, da0;
    if (probe_kernel_read(&hdr, (u8 *)rfb + rfb_hdr, sizeof hdr) || probe_kernel_read(&rcpi, hdr + hdr_rcpi, 1)
        || probe_kernel_read(&pad, hdr + 4, 1) || probe_kernel_read(&da0, hdr + hdr_len + (pad & 3), 1)) return;
    if (!rcpi || rcpi == 255) return;
    atomic_set(&last, rcpi | (da0 & 1) << 8); ACCESS_ONCE(last_jiffies) = jiffies; atomic_inc(&frames);
}

/* AAPCS: r0-r3, ip, lr are the caller's to lose; six words keep sp 8-byte aligned for the call */
void hm_tramp(void);
asm(".text\n.arm\n.align 2\n.global hm_tramp\nhm_tramp:\n"
    "  push {r0, r1, r2, r3, ip, lr}\n"
    "  bl hm_rx\n"
    "  pop {r0, r1, r2, r3, ip, lr}\n"
    "  push {r3, r4, r5, r6, r7, r8, r9, lr}\n"      /* the displaced instruction (hook_insn, checked) */
    "  ldr ip, =hm_ret\n"
    "  ldr pc, [ip]\n"
    ".ltorg\n");

static int patch(void *insn)
{
    *hook_at = *(u32 *)insn;
    flush_icache_range((unsigned long)hook_at, (unsigned long)hook_at + 4);
    return 0;
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

static int __init init_fn(void)
{
    u32 insn; long off;
    hook_at = (u32 *)kallsyms_lookup_name(hook_sym);
    if (!hook_at) { pr_err("hassmic_rcpi: %s not in this kernel\n", hook_sym); return -ENOENT; }
    if (probe_kernel_read(&insn, hook_at, 4) || insn != hook_insn) {
        pr_err("hassmic_rcpi: %s starts with %08x, not %08x: not patched\n", hook_sym, insn, hook_insn); return -EINVAL;
    }
    off = ((long)hm_tramp - ((long)hook_at + 8)) >> 2;
    if (off < -(1L << 23) || off >= (1L << 23)) { pr_err("hassmic_rcpi: hook out of branch range\n"); return -ERANGE; }
    if (!proc_create("hassmic_rcpi", 0444, NULL, &fops)) return -ENOMEM;
    hm_ret = (unsigned long)hook_at + 4;
    insn = 0xea000000 | (off & 0xffffff);                       /* b hm_tramp */
    stop_machine(patch, &insn, NULL);
    pr_info("hassmic_rcpi: %s at %p hooked, /proc/hassmic_rcpi\n", hook_sym, hook_at);
    return 0;
}

module_init(init_fn);
MODULE_LICENSE("GPL");      /* kallsyms_lookup_name and stop_machine are GPL-only */
MODULE_DESCRIPTION("RCPI of the frames from the access point, from MediaTek's gen2 Wi-Fi driver, for hassmic's Wi-Fi motion");
