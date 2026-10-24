/*
 * gxctl - bring-up tool for the Goodix 27c6:5125.
 *
 *   gxctl info              plain init, no TLS (safe, read-only)
 *   gxctl tls               init + TLS handshake with the PSK
 *   gxctl capture [N] [DIR] capture N fingerprints as PGM files
 *   gxctl unlock            release a sensor stuck in a TLS handshake
 *
 * Options: -v (trace frames), -k FILE (PSK, default ./psk.bin or
 * /etc/goodix5125/psk.bin)
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include "gx5125.h"

static void log_cb(void *ctx, const char *fmt, ...)
{
    va_list ap;

    (void)ctx;
    va_start(ap, fmt);
    fputs("  | ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static unsigned wait_ms(void)
{
    const char *env = getenv("GX_WAIT_MS");

    return env ? (unsigned)atoi(env) : 60000;
}

static int load_psk(const char *path, uint8_t psk[GX_PSK_LEN])
{
    static const char *const defaults[] = { "psk.bin", "/etc/goodix5125/psk.bin" };

    if (path)
        return gx_psk_load(path, psk);
    for (size_t i = 0; i < 2; i++)
        if (gx_psk_load(defaults[i], psk) == GX_OK)
            return GX_OK;
    return GX_ERR_IO;
}

static void print_hex(const char *label, const uint8_t *b, size_t n)
{
    printf("%-12s", label);
    for (size_t i = 0; i < n; i++)
        printf("%02x", b[i]);
    printf("\n");
}

static int cmd_info(gx_dev *dev)
{
    gx_info info;
    int rc = gx_sensor_init(dev, &info);

    printf("firmware    %s\n", info.firmware);
    print_hex("psk hash", info.psk_hash, 32);
    print_hex("otp", info.otp, info.otp_len);
    printf("otp valid   %s\n", gx_otp_valid(info.otp, info.otp_len) ? "yes" : "no");
    printf("calibration %04x %04x %04x %04x\n", info.calibration[0],
           info.calibration[1], info.calibration[2], info.calibration[3]);
    return rc;
}

static int cmd_tls(gx_dev *dev, const uint8_t *psk)
{
    gx_info info;
    int rc = gx_sensor_open(dev, &info, psk);

    if (rc == GX_OK)
        printf("TLS channel established\n");
    gx_sensor_close(dev);
    return rc;
}

static int cmd_background(gx_dev *dev, const uint8_t *psk, const char *dir)
{
    static uint16_t bg[GX_IMG_PIXELS];
    char path[512];
    gx_info info;
    unsigned min = 4095, max = 0;
    double sum = 0;
    int rc;

    mkdir(dir, 0700);
    rc = gx_sensor_open(dev, &info, psk);
    if (rc == GX_OK)
        rc = gx_sensor_background(dev, &info, bg);
    if (rc == GX_OK) {
        for (int y = 0; y < GX_IMG_H; y++)
            for (int x = 0; x < GX_CROP_W; x++) {
                unsigned v = bg[y * GX_IMG_W + x];
                min = v < min ? v : min;
                max = v > max ? v : max;
                sum += v;
            }
        printf("background: min %u max %u mean %.0f; dead column 70 = %u\n", min, max,
               sum / (GX_CROP_W * GX_IMG_H), bg[10 * GX_IMG_W + 70]);
        snprintf(path, sizeof(path), "%s/background.pgm", dir);
        gx_pgm_write16(path, bg, GX_IMG_W, GX_IMG_H);
    }
    gx_sensor_close(dev);
    return rc;
}

static int cmd_capture(gx_dev *dev, const uint8_t *psk, int count, const char *dir)
{
    static uint16_t bg[GX_IMG_PIXELS], fp[GX_IMG_PIXELS];
    static uint8_t img[GX_CROP_W * GX_CROP_H];
    char path[512];
    gx_info info;
    int rc;

    mkdir(dir, 0700);
    rc = gx_sensor_open(dev, &info, psk);
    if (rc < 0)
        goto out;

    rc = gx_sensor_background(dev, &info, bg);
    if (rc < 0)
        goto out;
    snprintf(path, sizeof(path), "%s/background.pgm", dir);
    gx_pgm_write16(path, bg, GX_IMG_W, GX_IMG_H);
    printf("background captured\n");

    for (int i = 0; i < count; i++) {
        printf("[%d/%d] touch the sensor...\n", i + 1, count);
        fflush(stdout);
        if ((rc = gx_sensor_wait_finger(dev, &info, wait_ms())) < 0)
            goto out;
        if ((rc = gx_sensor_capture(dev, fp)) < 0)
            goto out;

        snprintf(path, sizeof(path), "%s/raw-%02d.pgm", dir, i);
        gx_pgm_write16(path, fp, GX_IMG_W, GX_IMG_H);
        gx_image_prepare(fp, bg, img);
        snprintf(path, sizeof(path), "%s/finger-%02d.pgm", dir, i);
        gx_pgm_write(path, img, GX_CROP_W, GX_CROP_H);
        printf("        captured, coverage %.2f, lift your finger\n",
               gx_image_coverage_raw(fp, bg));
        fflush(stdout);

        if ((rc = gx_sensor_wait_lift(dev, &info, wait_ms())) < 0)
            goto out;
    }

out:
    gx_sensor_close(dev);
    return rc;
}

int main(int argc, char **argv)
{
    const char *psk_path = NULL;
    uint8_t psk[GX_PSK_LEN];
    gx_transport t;
    gx_usb *usb;
    gx_dev *dev;
    int opt, rc;

    while ((opt = getopt(argc, argv, "vk:")) != -1) {
        if (opt == 'k')
            psk_path = optarg;
        else if (opt != 'v') {
            fprintf(stderr, "usage: %s [-v] [-k psk] info|tls|capture [N] [DIR]|unlock\n", argv[0]);
            return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: %s [-v] [-k psk] info|tls|capture [N] [DIR]|unlock\n", argv[0]);
        return 2;
    }
    const char *cmd = argv[optind];

    if (gx_usb_open(&usb, &t) < 0) {
        fprintf(stderr, "cannot open 27c6:5125 (udev rule installed? fprintd running?)\n");
        return 1;
    }
    dev = calloc(1, sizeof(*dev));
    gx_dev_init(dev, &t);
    dev->log = log_cb;
    optind = 1;
    while ((opt = getopt(argc, argv, "vk:")) != -1)
        if (opt == 'v')
            dev->trace = true;

    if (!strcmp(cmd, "info")) {
        rc = cmd_info(dev);
    } else if (!strcmp(cmd, "unlock")) {
        static const uint8_t none[2];
        gx_drain(dev);
        rc = gx_command(dev, GX_CMD_TLS_ESTABLISHED, none, 2, NULL, 0, NULL);
        gx_drain(dev);
    } else if (!strcmp(cmd, "tls") || !strcmp(cmd, "capture") || !strcmp(cmd, "bg")) {
        if ((rc = load_psk(psk_path, psk)) < 0) {
            fprintf(stderr, "cannot read PSK (-k FILE)\n");
        } else if (!strcmp(cmd, "bg")) {
            rc = cmd_background(dev, psk, optind + 1 < argc ? argv[optind + 1] : "captures");
        } else if (!strcmp(cmd, "tls")) {
            rc = cmd_tls(dev, psk);
        } else {
            int n = optind + 1 < argc ? atoi(argv[optind + 1]) : 1;
            const char *dir = optind + 2 < argc ? argv[optind + 2] : "captures";
            rc = cmd_capture(dev, psk, n > 0 ? n : 1, dir);
        }
        OPENSSL_cleanse(psk, sizeof(psk));
    } else {
        fprintf(stderr, "unknown command %s\n", cmd);
        rc = GX_ERR_ARG;
    }

    if (rc < 0)
        fprintf(stderr, "error: %s\n", gx_strerror(rc));
    free(dev);
    gx_usb_close(usb);
    return rc < 0 ? 1 : 0;
}
