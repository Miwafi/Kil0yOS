/* USB HID boot-protocol drivers: keyboard + mouse.
 *
 * Reports arrive from the UHCI interrupt IN pipe (via usb_tick polling,
 * ~100 Hz, IF=0 context).  The keyboard driver diffs each report against
 * the previous one and replays the edges as set-1 scancodes through
 * keyboard_feed_scancode(), so the existing line-discipline (shift/caps/
 * ctrl, extended keys, ring buffer) is reused byte-for-byte.  While a
 * USB device is bound, its PS/2 counterpart is suppressed (IRQ handler
 * drains and discards); unbinding restores it.
 */
#include "usb/usb.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "timer/pit.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "drivers/vga.h"

/* HID usage ID -> set-1 make scancode.  Bit 7 marks extended keys
 * (delivered as keyboard_feed_scancode(sc, 1)). */
#define UEXT 0x80
static const uint8_t usage2sc[256] = {
    /* 0x04..0x1D: a..z */
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20,
    [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,
    [0x1C] = 0x15, [0x1D] = 0x2C,
    /* 0x1E..0x27: 1..9,0 */
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05,
    [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09,
    [0x26] = 0x0A, [0x27] = 0x0B,
    /* control block */
    [0x28] = 0x1C,                    /* Enter */
    [0x29] = 0x01,                    /* Esc */
    [0x2A] = 0x0E,                    /* Backspace */
    [0x2B] = 0x0F,                    /* Tab */
    [0x2C] = 0x39,                    /* Space */
    [0x2D] = 0x0C, [0x2E] = 0x0D,     /* - = */
    [0x2F] = 0x1A, [0x30] = 0x1B,     /* [ ] */
    [0x31] = 0x2B,                    /* \ */
    [0x33] = 0x27, [0x34] = 0x28,     /* ; ' */
    [0x35] = 0x29,                    /* ` */
    [0x36] = 0x33, [0x37] = 0x34, [0x38] = 0x35,   /* , . / */
    [0x39] = 0x3A,                    /* CapsLock */
    /* F1..F12 */
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E,
    [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,
    [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,
    /* keypad (non-extended) */
    [0x54] = 0x35, [0x55] = 0x37, [0x56] = 0x4A, [0x57] = 0x4E,
    [0x58] = 0x1C, [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51,
    [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47,
    [0x60] = 0x48, [0x61] = 0x49, [0x62] = 0x52, [0x63] = 0x53,
    /* extended block */
    [0x4C] = 0x53 | UEXT,             /* Delete */
    [0x4F] = 0x4D | UEXT,             /* Right */
    [0x50] = 0x4B | UEXT,             /* Left */
    [0x51] = 0x50 | UEXT,             /* Down */
    [0x52] = 0x48 | UEXT,             /* Up */
};

/* byte0 modifier bits -> set-1 make scancodes (0 = ignore, e.g. GUI) */
static const uint8_t mod2sc[8] = { 29, 42, 56, 0, 29, 54, 56, 0 };

/* --- keyboard ----------------------------------------------------------- */

static usb_device_t* kbd_dev;
static uint8_t kbd_prev[8];
static int kbd_seen_first;

/* bring-up probe state (see usb_hid_probe_tick) */
static uint32_t kbd_last_xfer;
static int kbd_silent;

/* --- mouse ---------------------------------------------------------------- */

static usb_device_t* mouse_dev;
static int mouse_seen_first;
static uint64_t mouse_last_log_us;

/* bring-up probe state (see usb_hid_probe_tick) */
static uint32_t mouse_last_xfer;
static int mouse_silent;

static int slot_has_usage(const uint8_t* r, uint8_t u) {
    for (int i = 2; i < 8; i++)
        if (r[i] == u) return 1;
    return 0;
}

static void kbd_report(usb_device_t* dev, const uint8_t* data, int len) {
    (void)dev;
    if (len > 8) len = 8;
    uint8_t r[8] = {0};
    memcpy(r, data, (size_t)len);

    if (r[2] == 0x01) return;             /* ErrorRollOver - hold state */

    if (!kbd_seen_first) {
        kbd_seen_first = 1;
        klog("[usb] usb_kbd_ok\n");
    }

    /* modifier edges */
    uint8_t diff = (uint8_t)(r[0] ^ kbd_prev[0]);
    for (int b = 0; b < 8; b++) {
        if (!(diff & (1u << b))) continue;
        uint8_t sc = mod2sc[b];
        if (sc == 0) continue;
        if (r[0] & (1u << b)) keyboard_feed_scancode(sc, 0);
        else                  keyboard_feed_scancode((uint8_t)(sc | 0x80), 0);
    }

    /* key-slot edges: make for new usage IDs, break for vanished ones */
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (u == 0 || slot_has_usage(kbd_prev, u)) continue;
        uint8_t e = usage2sc[u];
        if (e & 0x7F) keyboard_feed_scancode((uint8_t)(e & 0x7F), e & UEXT);
    }
    for (int i = 2; i < 8; i++) {
        uint8_t u = kbd_prev[i];
        if (u == 0 || slot_has_usage(r, u)) continue;
        uint8_t e = usage2sc[u];
        if (e & 0x7F)
            keyboard_feed_scancode((uint8_t)((e & 0x7F) | 0x80), e & UEXT);
    }

    memcpy(kbd_prev, r, sizeof(kbd_prev));
}

/* --- mouse --------------------------------------------------------------- */

static void mouse_report(usb_device_t* dev, const uint8_t* data, int len) {
    (void)dev;
    if (len < 3) return;
    int dx = (int8_t)data[1];
    int dy = (int8_t)data[2];
    int btn = data[0] & 0x07;

    if (!mouse_seen_first) {
        mouse_seen_first = 1;
        mouse_last_log_us = pit_uptime_us();
        char b[96];
        ksprintf(b, sizeof(b),
                 "[usb] usb_mouse_delta dx=%d dy=%d btn=%d\n",
                 dx, dy, btn);
        klog(b);
    } else if (dx || dy || btn) {
        uint64_t now = pit_uptime_us();
        if (now - mouse_last_log_us >= 500000) {   /* <= 2 lines/s */
            mouse_last_log_us = now;
            char b[96];
            ksprintf(b, sizeof(b),
                     "[usb] usb_mouse_delta dx=%d dy=%d btn=%d\n",
                     dx, dy, btn);
            klog(b);
        }
    }
    mouse_inject_delta(dx, dy, btn);
}

/* --- bind / unbind -------------------------------------------------------- */

void usb_hid_attach(usb_device_t* dev) {
    if (dev->if_protocol == 1) {
        if (kbd_dev != NULL) {
            klog("[usb] second HID keyboard ignored\n");
            return;
        }
        kbd_dev = dev;
        kbd_seen_first = 0;
        kbd_last_xfer = 0;
        kbd_silent = 0;
        dev->on_report = kbd_report;
        keyboard_set_ps2_enabled(0);
        klog("[usb] usb_ps2_suppressed\n");
    } else if (dev->if_protocol == 2) {
        if (mouse_dev != NULL) {
            klog("[usb] second HID mouse ignored\n");
            return;
        }
        mouse_dev = dev;
        mouse_seen_first = 0;
        mouse_last_xfer = 0;
        mouse_silent = 0;
        dev->on_report = mouse_report;
        mouse_set_ps2_enabled(0);
        klog("[usb] usb_mouse_ok\n");
    }
}

void usb_hid_detach(usb_device_t* dev) {
    if (dev == kbd_dev) {
        kbd_dev = NULL;
        memset(kbd_prev, 0, sizeof(kbd_prev));
        keyboard_set_ps2_enabled(1);
        klog("[usb] usb_kbd_gone\n");
        klog("[usb] usb_ps2_resumed\n");
    } else if (dev == mouse_dev) {
        mouse_dev = NULL;
        mouse_seen_first = 0;
        mouse_set_ps2_enabled(1);
        klog("[usb] usb_mouse_gone\n");
        klog("[usb] usb_ps2_resumed\n");
    }
}

/* --- bring-up probe -------------------------------------------------------- */

/* A bound HID device whose interrupt pipe stays fully silent for 3 s
 * (no reports AND no NAKs - a live-but-idle device keeps NAKing the
 * polled IN token, so nak_count advances) will never deliver input:
 * unbind it and give the console back to the PS/2 counterpart.  Covers
 * the parked-firmware case where a virtual USB HID enumerates fine but
 * its pipe is dead - the unconditional keyboard_set_ps2_enabled(0) in
 * usb_hid_attach then silenced the only working keyboard.  Called from
 * usb_tick at the ~1 s cadence, IF=0. */
#define HID_PROBE_TICKS 3

void usb_hid_probe_tick(void) {
    if (kbd_dev) {
        uint32_t xfer = kbd_dev->nak_count + kbd_dev->report_count;
        if (xfer != kbd_last_xfer) {
            kbd_last_xfer = xfer;
            kbd_silent = 0;
        } else if (kbd_dev->hcd != USB_HCD_XHCI &&
                   ++kbd_silent >= HID_PROBE_TICKS) {   /* xHCI: no NAK
                     counting; an idle keyboard is normal, never unbind */
            klog("[usb] usb_kbd pipe silent - releasing bind\n");
            usb_hid_detach(kbd_dev);          /* restores the PS/2 keyboard */
        }
    }
    if (mouse_dev) {
        uint32_t xfer = mouse_dev->nak_count + mouse_dev->report_count;
        if (xfer != mouse_last_xfer) {
            mouse_last_xfer = xfer;
            mouse_silent = 0;
        } else if (mouse_dev->hcd != USB_HCD_XHCI &&
                   ++mouse_silent >= HID_PROBE_TICKS) {
            klog("[usb] usb_mouse pipe silent - releasing bind\n");
            usb_hid_detach(mouse_dev);        /* restores the PS/2 mouse */
        }
    }
}
