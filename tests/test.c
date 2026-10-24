/*
 * Hardware-free tests. Reference bytes were recorded from the real 5125 by
 * tools/probe.py.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "gx5125.h"

static int failures, checks;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static size_t unhex(const char *s, uint8_t *out)
{
    size_t n = 0;

    for (; s[0] && s[1]; s += 2)
        sscanf(s, "%2hhx", &out[n++]);
    return n;
}

static void test_framing(void)
{
    uint8_t ref[64], msg[64], pack[64];
    const uint8_t read_chip_id[4] = { 0x00, 0x00, 0x00, 0x04 };
    size_t ref_len = unhex("a00800a8820500000000041f", ref);
    size_t m = gx_msg_encode(GX_CMD_READ_REG, read_chip_id, 4, true, msg, sizeof(msg));
    size_t p = gx_pack_encode(GX_FLAGS_MSG, msg, m, pack, sizeof(pack));

    CHECK(p == ref_len && memcmp(pack, ref, p) == 0);

    /* NOP carries the 0x88 marker instead of a checksum. */
    ref_len = unhex("a00800a80005000000000088", ref);
    m = gx_msg_encode(GX_CMD_NOP, (const uint8_t[4]){ 0 }, 4, false, msg, sizeof(msg));
    p = gx_pack_encode(GX_FLAGS_MSG, msg, m, pack, sizeof(pack));
    CHECK(p == ref_len && memcmp(pack, ref, p) == 0);

    /* Chip id reply. */
    uint8_t flags, cmd;
    const uint8_t *body, *payload;
    size_t body_len, payload_len;
    ref_len = unhex("a00800a8820500a204250058", ref);
    CHECK(gx_pack_decode(ref, ref_len, &flags, &body, &body_len) == (int)ref_len);
    CHECK(flags == GX_FLAGS_MSG);
    CHECK(gx_msg_decode(body, body_len, &cmd, &payload, &payload_len) == GX_OK);
    CHECK(cmd == GX_CMD_READ_REG && payload_len == 4);
    CHECK(memcmp(payload, "\xa2\x04\x25\x00", 4) == 0);

    /* Incomplete and corrupt packs. */
    CHECK(gx_pack_decode(ref, 6, NULL, NULL, NULL) == 0);
    ref[3] ^= 1;
    CHECK(gx_pack_decode(ref, ref_len, NULL, NULL, NULL) < 0);

    /* Firmware version reply, checksummed. */
    ref_len = unhex("a01a00baa8170047465f53543431315345435f4150505f31323530390047", ref);
    CHECK(gx_pack_decode(ref, ref_len, &flags, &body, &body_len) > 0);
    CHECK(gx_msg_decode(body, body_len, &cmd, &payload, &payload_len) == GX_OK);
    CHECK(cmd == GX_CMD_FW_VERSION && !strcmp((const char *)payload, "GF_ST411SEC_APP_12509"));
}

/* Scripted transport: read() hands out the queued USB transfers in order. */
typedef struct {
    uint8_t chunk[8][256];
    size_t len[8];
    int n, next;
} fake_usb;

static int fake_write(void *ctx, const uint8_t *buf, size_t len, unsigned timeout)
{
    return GX_OK;
}

static int fake_read(void *ctx, uint8_t *buf, size_t cap, size_t *got, unsigned timeout)
{
    fake_usb *f = ctx;

    *got = 0;
    if (f->next == f->n)
        return GX_ERR_TIMEOUT;
    memcpy(buf, f->chunk[f->next], f->len[f->next]);
    *got = f->len[f->next++];
    return GX_OK;
}

/* Appends one message pack to transfer i, zero-padded to 64 bytes if asked. */
static void fake_msg(fake_usb *f, int i, uint8_t cmd, const uint8_t *p, size_t len,
                     bool pad)
{
    uint8_t msg[128];
    size_t m = gx_msg_encode(cmd, p, len, true, msg, sizeof(msg));

    f->len[i] += gx_pack_encode(GX_FLAGS_MSG, msg, m, f->chunk[i] + f->len[i],
                                sizeof(f->chunk[i]) - f->len[i]);
    if (pad)
        f->len[i] = (f->len[i] + 63) / 64 * 64;
    if (i >= f->n)
        f->n = i + 1;
}

static void test_command(void)
{
    static fake_usb f;
    static gx_dev dev;
    gx_transport t = { fake_write, fake_read, &f, true };
    uint8_t reply[16];
    size_t n = 0;

    /* A late ACK for an earlier command (TLS_ESTABLISHED) is not ours. */
    memset(&f, 0, sizeof(f));
    gx_dev_init(&dev, &t);
    fake_msg(&f, 0, GX_CMD_ACK, (const uint8_t[]){ GX_CMD_TLS_ESTABLISHED, 0x01 }, 2, true);
    fake_msg(&f, 1, GX_CMD_ACK, (const uint8_t[]){ GX_CMD_QUERY_MCU_STATE, 0x01 }, 2, true);
    fake_msg(&f, 2, GX_CMD_QUERY_MCU_STATE, (const uint8_t[]){ 0x5a }, 1, true);
    CHECK(gx_command(&dev, GX_CMD_QUERY_MCU_STATE, (const uint8_t[]){ 0x55 }, 1,
                     reply, sizeof(reply), &n) == GX_OK);
    CHECK(n == 1 && reply[0] == 0x5a);

    /* One transfer: a padded pack, then the next one. */
    memset(&f, 0, sizeof(f));
    gx_dev_init(&dev, &t);
    fake_msg(&f, 0, GX_CMD_ACK, (const uint8_t[]){ GX_CMD_READ_REG, 0x01 }, 2, true);
    fake_msg(&f, 0, GX_CMD_READ_REG, (const uint8_t[]){ 0xa2, 0x04, 0x25, 0x00 }, 4, false);
    CHECK(gx_command(&dev, GX_CMD_READ_REG, (const uint8_t[]){ 0, 0, 0, 4 }, 4,
                     reply, sizeof(reply), &n) == GX_OK);
    CHECK(n == 4 && memcmp(reply, "\xa2\x04\x25\x00", 4) == 0);
}

static void test_otp(void)
{
    uint8_t otp[64];
    uint16_t cal[4];

    unhex("5333533035312e000a7580b1b076d8095107050c7d2bc7274103980cf1807f0c"
          "07c13043e280dcfc040ebc4302bcbcbebcbcbcbebcbc3546e21ddc231a45c7ae", otp);
    CHECK(gx_otp_valid(otp, 64));
    gx_otp_calibration(otp, cal);
    CHECK(cal[0] == 0x0bc8 && cal[1] == 0xbe && cal[2] == 0xbc && cal[3] == 0xbc);

    otp[47] ^= 0x10;
    CHECK(!gx_otp_valid(otp, 64));
}

static void test_decode(void)
{
    static uint8_t raw[GX_IMG_RAW_LEN];
    static uint16_t px[GX_IMG_PIXELS];

    /* One group of four known samples: 0x123, 0x456, 0x789, 0xabc. */
    uint8_t *c = raw + 8;
    c[0] = 0x61; c[1] = 0x23; c[3] = 0x45;
    c[2] = 0x89; c[5] = 0xc7; c[4] = 0xab;
    gx_image_decode(raw, px);
    CHECK(px[0] == 0x123 && px[1] == 0x456 && px[2] == 0x789 && px[3] == 0xabc);
}

static void test_empty(void)
{
    static uint16_t px[GX_IMG_PIXELS];

    for (int i = 0; i < GX_IMG_PIXELS; i++)
        px[i] = 2780 + (i % 3) * 20;
    CHECK(gx_image_is_empty(px));
    /* Finger: lower mean and strong ridge texture (period 6). */
    for (int i = 0; i < GX_IMG_PIXELS; i++)
        px[i] = (uint16_t)(1950 + ((i % GX_IMG_W) % 6 < 3 ? 150 : -150));
    CHECK(!gx_image_is_empty(px));
}

static void test_psk_file(void)
{
    char path[] = "/tmp/gx-psk-XXXXXX";
    uint8_t psk[GX_PSK_LEN];
    FILE *f;
    int fd = mkstemp(path);

    f = fdopen(fd, "w");
    fputs("00112233445566778899aabbccddeeff\n00112233445566778899AABBCCDDEEFF\n", f);
    fclose(f);
    CHECK(gx_psk_load(path, psk) == GX_OK);
    CHECK(psk[0] == 0x00 && psk[5] == 0x55 && psk[15] == 0xff && psk[31] == 0xff);

    f = fopen(path, "w");
    fputs("0011", f);
    fclose(f);
    CHECK(gx_psk_load(path, psk) < 0);
    unlink(path);
}

/* Synthetic print: curved ridges plus pores at seeded positions, standing
 * in for real captures (which are biometric data and stay out of the repo). */
static void synth(uint8_t *img, unsigned seed, double tilt, double bend)
{
    double pores[40][2];

    srand(seed);
    for (int i = 0; i < 40; i++) {
        pores[i][0] = rand() % GX_CROP_W;
        pores[i][1] = rand() % GX_CROP_H;
    }
    for (int y = 0; y < GX_CROP_H; y++)
        for (int x = 0; x < GX_CROP_W; x++) {
            double u = x * cos(tilt) + y * sin(tilt) + bend * sin(y / 13.0) + 0.02 * (x - 32) * (x - 32) * bend / 6;
            double v = 128 + 70 * sin(2 * M_PI * u / 7.0);
            for (int i = 0; i < 40; i++) {
                double d = hypot(x - pores[i][0], y - pores[i][1]);
                if (d < 2.0)
                    v = (i & 1) ? 250 : 5;
            }
            img[y * GX_CROP_W + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

static void rotate_img(const uint8_t *in, uint8_t *out, double deg)
{
    double c = cos(deg * M_PI / 180), s = sin(deg * M_PI / 180);
    double cx = (GX_CROP_W - 1) / 2.0, cy = (GX_CROP_H - 1) / 2.0;

    for (int y = 0; y < GX_CROP_H; y++)
        for (int x = 0; x < GX_CROP_W; x++) {
            int sx = (int)lround(c * (x - cx) + s * (y - cy) + cx);
            int sy = (int)lround(-s * (x - cx) + c * (y - cy) + cy);
            out[y * GX_CROP_W + x] = sx >= 0 && sy >= 0 && sx < GX_CROP_W && sy < GX_CROP_H
                                     ? in[sy * GX_CROP_W + sx] : 128;
        }
}

static void test_match(void)
{
    static gx_template a, b;

    synth(a.img, 1, 0.6, 2.0);
    b = a;
    CHECK(gx_match(&a, &b) >= 20);

    rotate_img(a.img, b.img, 30);
    int rotated = gx_match(&a, &b);
    CHECK(rotated >= GX_MATCH_THRESHOLD);

    synth(b.img, 2, 1.1, -1.5);
    int other = gx_match(&a, &b);
    CHECK(other < GX_MATCH_THRESHOLD);

    if (getenv("VERBOSE"))
        printf("match: rotated %d, other %d\n", rotated, other);
}

int main(void)
{
    test_framing();
    test_command();
    test_otp();
    test_decode();
    test_psk_file();
    test_empty();
    test_match();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
