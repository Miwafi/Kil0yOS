/* Host test for the HDA codec layer: drives drivers/hda.c's discovery and
 * path setup against a canned Realtek ALC662 topology (no controller, no
 * MMIO), the same way tools/pkg_host_test.c drives the package code.
 *
 * Topology modelled (ALC662 desktop board):
 *   0x02/0x03/0x04  DACs            0x0c front mixer   0x0d mixer
 *   0x14 line-out (assoc 1, EAPD) -> 0x0c -> 0x02
 *   0x15 hp-out   (assoc 1, EAPD) -> 0x0d -> 0x03
 *   0x18 mic-in, 0x1e "no connection" pin -> must be ignored
 * Scenario 2 moves the 0x15 pin into association 2, where it must drop out of
 * the stream's DAC set.
 *
 * Build+run:  gcc -O1 -DHDA_HOST_TEST -I. -o /tmp/hda_test tools/hda_host_test.c
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void klog(const char* s) { fputs(s, stdout); }

void ksprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, size, fmt, ap);
    va_end(ap);
}

/* ---- canned codec: verb table ---------------------------------------- */
#define NOK 0xff

typedef struct {
    uint8_t  nid;
    uint16_t verb;        /* 12-bit verb id (0 = unused entry) */
    uint8_t  payload;
    uint32_t resp;
} fake_t;

#define PARAMETRIC 0x0f00

/* ALC662: 3 DACs, 2 mixers, line-out + hp-out + ignored pins */
static fake_t codec[] = {
    /* root */
    { 0x00, PARAMETRIC, 0x00, 0x10ec0662 },            /* vendor id */
    { 0x00, PARAMETRIC, 0x02, 0x00100300 },            /* revision id (rev3) */
    { 0x00, PARAMETRIC, 0x04, 0x00010001 },            /* 1 node, start 1 */
    /* audio function group */
    { 0x01, PARAMETRIC, 0x05, 0x01 },                  /* function type = audio */
    { 0x01, PARAMETRIC, 0x04, 0x0002001d },            /* 29 nodes, start 0x02 */
    /* widgets: 0x02..0x1e (only the interesting ones carry data) */
    { 0x02, PARAMETRIC, 0x09, (0u << 20) | 0x0415 },   /* DAC: stereo|out-amp|fmt|power */
    { 0x03, PARAMETRIC, 0x09, (0u << 20) | 0x0415 },
    { 0x04, PARAMETRIC, 0x09, (0u << 20) | 0x0415 },
    { 0x07, PARAMETRIC, 0x09, (1u << 20) | 0x0015 },   /* ADC */
    { 0x02, PARAMETRIC, 0x12, 0x80001f1f },            /* out amp caps */
    { 0x03, PARAMETRIC, 0x12, 0x80001f1f },
    { 0x04, PARAMETRIC, 0x12, 0x80001f1f },
    { 0x0c, PARAMETRIC, 0x09, (2u << 20) | 0x0507 },   /* front mixer */
    { 0x0c, PARAMETRIC, 0x0e, 3 },                     /* 3 connections */
    { 0x0c, PARAMETRIC, 0x12, 0x80001f1f },
    { 0x0c, PARAMETRIC, 0x0d, 0x80001f1f },            /* in amp caps */
    { 0x0d, PARAMETRIC, 0x09, (2u << 20) | 0x0507 },
    { 0x0d, PARAMETRIC, 0x0e, 1 },
    { 0x0d, PARAMETRIC, 0x12, 0x80001f1f },
    { 0x0d, PARAMETRIC, 0x0d, 0x80001f1f },
    /* front line-out */
    { 0x14, PARAMETRIC, 0x09, (4u << 20) | 0x0009 },
    { 0x14, PARAMETRIC, 0x0c, 0x00010014 },            /* pin caps: out|eapd */
    { 0x14, PARAMETRIC, 0x0e, 2 },                     /* 2 connections */
    { 0x14, PARAMETRIC, 0x12, 0x80001f1f },
    /* headphone / second output */
    { 0x15, PARAMETRIC, 0x09, (4u << 20) | 0x0009 },
    { 0x15, PARAMETRIC, 0x0c, 0x00010014 },
    { 0x15, PARAMETRIC, 0x0e, 1 },
    { 0x15, PARAMETRIC, 0x12, 0x80001f1f },
    /* mic in, and a pin with no physical connection */
    { 0x18, PARAMETRIC, 0x09, (4u << 20) | 0x0009 },
    { 0x18, PARAMETRIC, 0x0c, 0x00000020 },            /* input capable only */
    { 0x1e, PARAMETRIC, 0x09, (4u << 20) | 0x0009 },
    { 0x1e, PARAMETRIC, 0x0c, 0x00010010 },
    { 0, 0, 0, 0 },
};

/* config default: 0x14 = line-out(0) assoc 1, 0x15 = hp-out(2) assoc 1 or 2 */
static int hp_assoc = 1;

static int conn_list(uint8_t nid, int idx, uint32_t* out) {
    static const uint8_t c14[] = { 0x0c, 0x0d };
    static const uint8_t c15[] = { 0x0d };
    static const uint8_t c0c[] = { 0x02, 0x03, 0x04 };
    static const uint8_t c0d[] = { 0x03 };
    const uint8_t* list = NULL;
    int n = 0;
    switch (nid) {
        case 0x14: list = c14; n = 2; break;
        case 0x15: list = c15; n = 1; break;
        case 0x0c: list = c0c; n = 3; break;
        case 0x0d: list = c0d; n = 1; break;
        default: break;
    }
    if (list == NULL || idx >= n) { *out = 0; return 0; }
    /* short form: four 8-bit entries packed per response */
    uint32_t v = 0;
    for (int i = 0; i < 4 && idx + i < n; i++) v |= (uint32_t)list[idx + i] << (8 * i);
    *out = v;
    return 1;
}

/* ---- executed SET verbs, for assertions ------------------------------ */
typedef struct { uint8_t nid; uint16_t verb; uint32_t payload; } setverb_t;
static setverb_t did[64];
static int did_count;

static int did_has(uint8_t nid, uint16_t verb) {
    for (int i = 0; i < did_count; i++) {
        if (did[i].nid == nid && did[i].verb == verb) return 1;
    }
    return 0;
}

static uint32_t did_get(uint8_t nid, uint16_t verb) {
    for (int i = 0; i < did_count; i++) {
        if (did[i].nid == nid && did[i].verb == verb) return did[i].payload;
    }
    return 0xdeadbeef;
}

/* first verb write to nid whose payload matches (mask, want) */
static uint32_t did_find(uint8_t nid, uint16_t verb, uint32_t mask, uint32_t want) {
    for (int i = 0; i < did_count; i++) {
        if (did[i].nid == nid && did[i].verb == verb && (did[i].payload & mask) == want) {
            return did[i].payload;
        }
    }
    return 0xdeadbeef;
}

static int hda_cmd(uint32_t cmd, uint32_t* resp) {
    uint8_t  nid   = (uint8_t)((cmd >> 20) & 0x7f);
    uint32_t data  = cmd & 0xfffff;
    uint32_t verb, payload;

    if ((data & 0x70000) == 0x70000) {          /* 12-bit verb, 8-bit payload */
        verb = (data >> 8) & 0xfff;
        payload = data & 0xff;
    } else {                                    /* 4-bit verb, 16-bit payload */
        verb = (data >> 8) & 0xf00;
        payload = data & 0xffff;
    }

    /* record SET verbs (including the long-form 0x200/0x300 amp and format) */
    int is_set = (verb >= 0x700 && verb <= 0x7ff) ||
                 verb == 0x200 || verb == 0x300 || verb == 0x400 || verb == 0x500;
    if (is_set) {
        if (did_count < (int)(sizeof(did) / sizeof(did[0]))) {
            did[did_count].nid = nid;
            did[did_count].verb = (uint16_t)verb;
            did[did_count].payload = payload;
            did_count++;
        }
        if (resp) *resp = 0;
        return 0;
    }

    uint32_t out = 0;
    if (verb == 0x0f1c) {                       /* get config default */
        if (nid == 0x14) out = (0u << 30) | (0u << 20) | (1u << 4);
        else if (nid == 0x15) out = (0u << 30) | (2u << 20) | ((uint32_t)hp_assoc << 4);
        else out = (0u << 30) | (0x0au << 20) | (1u << 4);
        if (resp) *resp = out;
        return 0;
    }
    if (verb == 0x0f02) {                       /* get connection list */
        uint32_t v = 0;
        conn_list(nid, (int)payload, &v);
        if (resp) *resp = v;
        return 0;
    }
    for (int i = 0; codec[i].verb != 0; i++) {
        if (codec[i].nid == nid && codec[i].verb == verb && codec[i].payload == payload) {
            if (resp) *resp = codec[i].resp;
            return 0;
        }
    }
    if (resp) *resp = 0;                        /* unspecified: 0 */
    return 0;
}

#include "../src/kernel/drivers/hda.c"

/* ---- assertions ------------------------------------------------------ */
static int fails;

static void check(int cond, const char* what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

static void scenario(int hp_assoc_value, int expect_pins, int expect_dacs) {
    hp_assoc = hp_assoc_value;
    did_count = 0;
    out_count = 0;
    dac_count = 0;

    printf("--- scenario: hp pin assoc %d ---\n", hp_assoc_value);
    fflush(stdout);
    check(hda_probe_codec(0) == 0, "probe succeeds");
    check(afg == 1, "audio function group = nid 1");
    check(out_count == expect_pins, "output pins with a DAC path");
    check(pin == 0x14, "primary pin = 0x14 (line-out)");
    check(dac == 0x02, "primary DAC = 0x02");
    check(dac_count == expect_dacs, "converters fed by the stream");
    check(dacs[0] == 0x02, "first stream DAC = 0x02");
    if (expect_dacs == 2) check(dacs[1] == 0x03, "second stream DAC = 0x03");
    check(strstr(codec_name, "ALC662") != NULL, "codec identified as ALC662");
    check(strstr(codec_name, "rev3") != NULL, "revision decoded (rev3)");

    check(did_has(0x14, 0x0707), "pin 0x14 enabled as output");
    check(did_has(0x14, 0x070c), "EAPD set on 0x14");
    check(did_has(0x0c, 0x0701) == 0, "front mixer needs no connect select");
    /* DAC 0x02 output amp unmuted (SET_OUTPUT, mute bit clear) */
    uint32_t amp = did_find(0x02, 0x0300, (1u << 15) | (1u << 7), 1u << 15);
    check(amp != 0xdeadbeef, "DAC 0x02 output amp unmuted");
    /* mixer 0x0c: input amp of connection 0 (the 0x02 DAC) unmuted */
    amp = did_find(0x0c, 0x0300, (1u << 14) | (0xfu << 8) | (1u << 7), 1u << 14);
    check(amp != 0xdeadbeef, "front mixer input amp 0 unmuted");
    if (expect_dacs == 2) {
        check(did_has(0x15, 0x0707), "sibling pin 0x15 enabled as output");
        check(did_has(0x15, 0x070c), "EAPD set on sibling 0x15");
        amp = did_find(0x0d, 0x0300, (1u << 14) | (0xfu << 8) | (1u << 7), 1u << 14);
        check(amp != 0xdeadbeef, "second mixer input amp 0 unmuted");
        amp = did_find(0x03, 0x0300, (1u << 15) | (1u << 7), 1u << 15);
        check(amp != 0xdeadbeef, "DAC 0x03 output amp unmuted");
    } else {
        check(!did_has(0x15, 0x070c), "pin in another association left alone");
        check(did_find(0x03, 0x0300, (1u << 15), 1u << 15) == 0xdeadbeef,
              "other association's DAC left alone");
    }
}

int main(void) {
    scenario(1, 2, 2);      /* front + hp in association 1: both play */
    scenario(2, 2, 1);      /* hp moved to association 2: only front plays */
    printf(fails ? "HDA_ALC662_TEST_FAILED (%d)\n" : "HDA_ALC662_TEST_OK\n", fails);
    return fails != 0;
}
