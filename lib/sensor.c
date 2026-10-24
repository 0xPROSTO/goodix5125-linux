/*
 * Sensor bring-up and capture sequences.
 *
 * Order and payloads follow the Windows driver as recorded by goodix-fp-dump
 * (driver_51x0.py, same GF_ST411SEC MCU and chip id a2 04 25 00) and were
 * checked against our 5125 with tools/probe.py.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdio.h>
#include <string.h>

#include "gx5125.h"

#define FW_PREFIX "GF_ST411SEC_APP_"
#define PSK_HASH_FLAGS 0xbb020003u
static const uint8_t CHIP_ID[4] = { 0xa2, 0x04, 0x25, 0x00 };

/* MCU configuration uploaded with 0x90 (opaque, from the Windows driver). */
static const uint8_t MCU_CONFIG[256] = {
    0x70, 0x11, 0x60, 0x71, 0x2c, 0x9d, 0x2c, 0xc9, 0x1c, 0xe5, 0x18, 0xfd,
    0x00, 0xfd, 0x00, 0xfd, 0x03, 0xba, 0x00, 0x01, 0x80, 0xca, 0x00, 0x04,
    0x00, 0x84, 0x00, 0x15, 0xb3, 0x86, 0x00, 0x00, 0xc4, 0x88, 0x00, 0x00,
    0xba, 0x8a, 0x00, 0x00, 0xb2, 0x8c, 0x00, 0x00, 0xaa, 0x8e, 0x00, 0x00,
    0xc1, 0x90, 0x00, 0xbb, 0xbb, 0x92, 0x00, 0xb1, 0xb1, 0x94, 0x00, 0x00,
    0xa8, 0x96, 0x00, 0x00, 0xb6, 0x98, 0x00, 0x00, 0x00, 0x9a, 0x00, 0x00,
    0x00, 0xd2, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00, 0x00, 0xd6, 0x00, 0x00,
    0x00, 0xd8, 0x00, 0x00, 0x00, 0x50, 0x00, 0x01, 0x05, 0xd0, 0x00, 0x00,
    0x00, 0x70, 0x00, 0x00, 0x00, 0x72, 0x00, 0x78, 0x56, 0x74, 0x00, 0x34,
    0x12, 0x20, 0x00, 0x10, 0x40, 0x2a, 0x01, 0x02, 0x04, 0x22, 0x00, 0x01,
    0x20, 0x24, 0x00, 0x32, 0x00, 0x80, 0x00, 0x01, 0x00, 0x5c, 0x00, 0x80,
    0x00, 0x56, 0x00, 0x04, 0x20, 0x58, 0x00, 0x03, 0x02, 0x32, 0x00, 0x0c,
    0x02, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0x58, 0x82, 0x00, 0x80,
    0x15, 0x2a, 0x01, 0x82, 0x03, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14,
    0x00, 0x80, 0x00, 0x01, 0x00, 0x5c, 0x00, 0x00, 0x01, 0x56, 0x00, 0x04,
    0x20, 0x58, 0x00, 0x03, 0x02, 0x32, 0x00, 0x0c, 0x02, 0x66, 0x00, 0x03,
    0x00, 0x7c, 0x00, 0x00, 0x58, 0x82, 0x00, 0x80, 0x1f, 0x2a, 0x01, 0x08,
    0x00, 0x5c, 0x00, 0x80, 0x00, 0x54, 0x00, 0x10, 0x01, 0x62, 0x00, 0x04,
    0x03, 0x64, 0x00, 0x19, 0x00, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x01,
    0x58, 0x2a, 0x01, 0x08, 0x00, 0x5c, 0x00, 0x00, 0x01, 0x52, 0x00, 0x08,
    0x00, 0x54, 0x00, 0x00, 0x01, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x01,
    0x58, 0x00, 0x89, 0x2e,
};

#define LOG(dev, ...) do { if ((dev)->log) (dev)->log((dev)->log_ctx, __VA_ARGS__); } while (0)

/* crcmod "crc-8": poly 0x07, init 0, not reflected. */
static uint8_t crc8(const uint8_t *buf, size_t len)
{
    uint8_t crc = 0;

    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
            crc = crc & 0x80 ? (uint8_t)(crc << 1 ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

static uint8_t crc8_parts(const uint8_t *otp, const int (*ranges)[2], int n)
{
    uint8_t tmp[64];
    size_t len = 0;

    for (int i = 0; i < n; i++)
        for (int j = ranges[i][0]; j < ranges[i][1]; j++)
            tmp[len++] = otp[j];
    return (uint8_t)~crc8(tmp, len);
}

/* Checksums of the factory OTP sections, as verified by the Windows driver. */
bool gx_otp_valid(const uint8_t *otp, size_t len)
{
    static const int cp[][2] = { { 0, 11 }, { 36, 40 } };
    static const int mt[][2] = { { 20, 28 }, { 29, 36 }, { 40, 50 }, { 54, 56 } };
    static const int ft[][2] = { { 11, 20 }, { 28, 29 }, { 50, 54 }, { 56, 60 }, { 62, 63 } };
    static const int dac_ft[][2] = { { 50, 54 } };
    static const int dac_mt[][2] = { { 46, 50 } };

    if (len < 64)
        return false;
    return crc8_parts(otp, cp, 2) == otp[60] &&
           crc8_parts(otp, mt, 4) == otp[63] &&
           crc8_parts(otp, ft, 5) == otp[61] &&
           crc8_parts(otp, dac_ft, 1) == otp[62] &&
           crc8_parts(otp, dac_mt, 1) == otp[22] &&
           memcmp(otp + 46, otp + 50, 4) == 0;
}

void gx_otp_calibration(const uint8_t *otp, uint16_t cal[4])
{
    cal[0] = (uint16_t)(otp[46] << 4 | 8);
    cal[1] = otp[47];
    cal[2] = otp[48];
    cal[3] = otp[49];
}

static int write_reg(gx_dev *dev, uint16_t addr, uint16_t value)
{
    const uint8_t p[5] = { 0x00, addr & 0xff, addr >> 8, value & 0xff, value >> 8 };

    return gx_command(dev, GX_CMD_WRITE_REG, p, sizeof(p), NULL, 0, NULL);
}

static int read_reg(gx_dev *dev, uint16_t addr, uint8_t len, uint8_t *out)
{
    const uint8_t p[4] = { 0x00, addr & 0xff, addr >> 8, len };
    uint8_t reply[64];
    size_t n = 0;
    int rc = gx_command(dev, GX_CMD_READ_REG, p, sizeof(p), reply, sizeof(reply), &n);

    if (rc < 0)
        return rc;
    if (n < len)
        return GX_ERR_PROTOCOL;
    if (out)
        memcpy(out, reply, len);
    return GX_OK;
}

static int reset_sensor(gx_dev *dev)
{
    /* bit0 reset sensor, bit2 likewise (as the Windows driver sends it),
     * then the sleep time in ms. */
    const uint8_t p[2] = { 0x05, 20 };
    uint8_t reply[8];
    size_t n = 0;
    int rc = gx_command(dev, GX_CMD_RESET, p, sizeof(p), reply, sizeof(reply), &n);

    if (rc < 0)
        return rc;
    if (n < 3 || reply[0] != 0x01)
        return GX_ERR_DEVICE;
    if ((reply[1] | reply[2] << 8) != 2048 && dev->log)
        dev->log(dev->log_ctx, "reset: unusual irq status %u",
                 reply[1] | reply[2] << 8);
    return GX_OK;
}

static int status_cmd(gx_dev *dev, uint8_t cmd, const uint8_t *p, size_t len)
{
    uint8_t reply[16];
    size_t n = 0;
    int rc = gx_command(dev, cmd, p, len, reply, sizeof(reply), &n);

    if (rc < 0)
        return rc;
    return n >= 1 && reply[0] == 0x01 ? GX_OK : GX_ERR_DEVICE;
}

int gx_sensor_init(gx_dev *dev, gx_info *info)
{
    static const uint8_t none[2];
    static const uint8_t enable[2] = { 0x01, 0x00 };
    static const uint8_t idle[2] = { 20, 0x00 };
    static const uint8_t freq[2] = { 100, 0x00 };
    uint8_t buf[256];
    size_t n = 0;
    int rc;

    memset(info, 0, sizeof(*info));
    gx_drain(dev);

    if ((rc = gx_nop(dev)) < 0)
        return rc;
    if ((rc = gx_command(dev, GX_CMD_ENABLE_CHIP, enable, 2, NULL, 0, NULL)) < 0)
        return rc;
    if ((rc = gx_nop(dev)) < 0)
        return rc;

    rc = gx_command(dev, GX_CMD_FW_VERSION, none, 2, buf, sizeof(buf) - 1, &n);
    if (rc < 0)
        return rc;
    buf[n] = '\0';
    snprintf(info->firmware, sizeof(info->firmware), "%.63s", (char *)buf);
    LOG(dev, "firmware %s", info->firmware);
    if (strncmp(info->firmware, FW_PREFIX, strlen(FW_PREFIX)) != 0)
        return GX_ERR_UNSUPPORTED;

    {
        const uint8_t p[8] = { PSK_HASH_FLAGS & 0xff, PSK_HASH_FLAGS >> 8 & 0xff,
                               PSK_HASH_FLAGS >> 16 & 0xff, PSK_HASH_FLAGS >> 24,
                               0, 0, 0, 0 };
        rc = gx_command(dev, GX_CMD_PSK_READ, p, sizeof(p), buf, sizeof(buf), &n);
        if (rc < 0)
            return rc;
        if (n >= 9 + 32 && buf[0] == 0x00)
            memcpy(info->psk_hash, buf + 9, 32);
    }

    if ((rc = reset_sensor(dev)) < 0)
        return rc;
    if ((rc = read_reg(dev, 0x0000, 4, buf)) < 0)
        return rc;
    if (memcmp(buf, CHIP_ID, 4) != 0) {
        LOG(dev, "unexpected chip id");
        return GX_ERR_UNSUPPORTED;
    }

    rc = gx_command(dev, GX_CMD_READ_OTP, none, 2, info->otp, sizeof(info->otp),
                    &info->otp_len);
    if (rc < 0)
        return rc;
    if (!gx_otp_valid(info->otp, info->otp_len)) {
        LOG(dev, "OTP checksum mismatch");
        return GX_ERR_DEVICE;
    }
    gx_otp_calibration(info->otp, info->calibration);

    if ((rc = reset_sensor(dev)) < 0)
        return rc;
    if ((rc = gx_command(dev, GX_CMD_IDLE, idle, 2, NULL, 0, NULL)) < 0)
        return rc;

    static const uint16_t regs[4] = { 0x0220, 0x0236, 0x0238, 0x023a };
    for (int i = 0; i < 4; i++)
        if ((rc = write_reg(dev, regs[i], info->calibration[i])) < 0)
            return rc;

    if ((rc = status_cmd(dev, GX_CMD_UPLOAD_CONFIG, MCU_CONFIG, sizeof(MCU_CONFIG))) < 0)
        return rc;
    return status_cmd(dev, GX_CMD_SCAN_FREQ, freq, 2);
}

int gx_sensor_open(gx_dev *dev, gx_info *info, const uint8_t psk[GX_PSK_LEN])
{
    int rc = gx_sensor_init(dev, info);

    if (rc < 0)
        return rc;
    return gx_tls_open(dev, psk);
}

void gx_sensor_close(gx_dev *dev)
{
    gx_tls_close(dev);
}

void gx_sensor_unstick(gx_dev *dev)
{
    static const uint8_t none[2];
    unsigned saved = dev->timeout_ms;

    gx_drain(dev);
    dev->timeout_ms = 500;
    gx_command(dev, GX_CMD_TLS_ESTABLISHED, none, sizeof(none), NULL, 0, NULL);
    dev->timeout_ms = saved;
    gx_drain(dev);
}

/* ---- capture ----------------------------------------------------------- */

/*
 * FDT ("finger detection") mode frames: op, 0x01, then six (flag, level)
 * pairs, one per detection zone. The MCU answers with its current raw zone
 * readings (six LE16 at payload offset 4); the Windows driver derives the
 * levels from those, so we do the same once a baseline is known.
 */
enum { FDT_OP_DOWN = 0x0c, FDT_OP_MODE = 0x0d, FDT_OP_UP = 0x0e };

/* Static fallbacks recorded from the Windows driver on a 5110. */
static const uint8_t FDT_BOOT[12] = { 0xae, 0xae, 0xbf, 0xbf, 0xa4, 0xa4,
                                      0xb8, 0xb8, 0xa8, 0xa8, 0xb7, 0xb7 };
static const uint8_t FDT_LEVELS[6] = { 0xaf, 0xbf, 0xa4, 0xb8, 0xa8, 0xb7 };

/* Records the zone readings of an FDT reply as levels (raw / 2). */
static int fdt_cmd(gx_dev *dev, gx_info *info, uint8_t cmd, const uint8_t mode[14],
                   bool learn)
{
    uint8_t reply[64];
    size_t n = 0;
    int rc = gx_command(dev, cmd, mode, 14, reply, sizeof(reply), &n);

    if (rc < 0)
        return rc;
    if (learn && n >= 16) {
        for (int k = 0; k < 6; k++) {
            unsigned raw = reply[4 + 2 * k] | reply[5 + 2 * k] << 8;
            info->fdt_level[k] = (uint8_t)(raw >> 1 > 255 ? 255 : raw >> 1);
        }
        info->fdt_level_valid = true;
        if (dev->log)
            dev->log(dev->log_ctx, "fdt levels %02x %02x %02x %02x %02x %02x",
                     info->fdt_level[0], info->fdt_level[1], info->fdt_level[2],
                     info->fdt_level[3], info->fdt_level[4], info->fdt_level[5]);
    }
    return GX_OK;
}

static void fdt_frame(const gx_info *info, uint8_t out[14], uint8_t op, int delta)
{
    const uint8_t *lv = info->fdt_level_valid ? info->fdt_level : FDT_LEVELS;

    out[0] = op;
    out[1] = 0x01;
    for (int k = 0; k < 6; k++) {
        int v = lv[k] + delta;
        out[2 + 2 * k] = 0x80;
        out[3 + 2 * k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

static int get_image(gx_dev *dev, uint16_t *pixels)
{
    static const uint8_t p[2] = { 0x01, 0x00 };
    uint8_t raw[GX_IMG_RAW_LEN];
    int rc = gx_command(dev, GX_CMD_GET_IMAGE, p, 2, NULL, 0, NULL);

    if (rc < 0)
        return rc;
    rc = gx_tls_read(dev, raw, sizeof(raw));
    if (rc < 0)
        return rc;
    gx_image_decode(raw, pixels);
    return GX_OK;
}

int gx_sensor_background(gx_dev *dev, gx_info *info, uint16_t *pixels)
{
    static const uint8_t mcu_state[1] = { 0x55 };
    static const uint8_t nav[2] = { 0x01, 0x00 };
    uint8_t reply[64], mode[14];
    size_t n;
    int rc;

    rc = gx_command(dev, GX_CMD_QUERY_MCU_STATE, mcu_state, 1, reply, sizeof(reply), &n);
    if (rc < 0)
        return rc;

    mode[0] = FDT_OP_MODE;
    mode[1] = 0x01;
    memcpy(mode + 2, FDT_BOOT, 12);
    if ((rc = fdt_cmd(dev, info, GX_CMD_FDT_MODE, mode, true)) < 0)
        return rc;

    if ((rc = gx_command(dev, GX_CMD_NAV, nav, 2, reply, sizeof(reply), &n)) < 0)
        return rc;

    fdt_frame(info, mode, FDT_OP_MODE, 0);
    if ((rc = fdt_cmd(dev, info, GX_CMD_FDT_MODE, mode, false)) < 0)
        return rc;

    if ((rc = read_reg(dev, 0x0082, 2, NULL)) < 0)
        return rc;

    return get_image(dev, pixels);
}

int gx_sensor_wait_finger(gx_dev *dev, gx_info *info, unsigned timeout_ms)
{
    uint8_t mode[14];
    unsigned saved = dev->timeout_ms;
    int rc;

    /* A detection armed by the previous wait_lift may have fired already. */
    gx_drain(dev);

    fdt_frame(info, mode, FDT_OP_MODE, 0);
    if ((rc = fdt_cmd(dev, info, GX_CMD_FDT_MODE, mode, false)) < 0)
        return rc;

    /* fdt_down only answers once a finger touches the sensor. */
    fdt_frame(info, mode, FDT_OP_DOWN, info->fdt_down_delta);
    dev->timeout_ms = timeout_ms;
    rc = fdt_cmd(dev, info, GX_CMD_FDT_DOWN, mode, false);
    dev->timeout_ms = saved;
    return rc;
}

int gx_sensor_capture(gx_dev *dev, uint16_t *pixels)
{
    return get_image(dev, pixels);
}

int gx_sensor_wait_lift(gx_dev *dev, gx_info *info, unsigned timeout_ms)
{
    static const uint8_t nav[2] = { 0x01, 0x00 };
    uint8_t mode[14], reply[64];
    unsigned saved = dev->timeout_ms;
    size_t n;
    int rc;

    fdt_frame(info, mode, FDT_OP_UP, 0);
    dev->timeout_ms = timeout_ms;
    rc = fdt_cmd(dev, info, GX_CMD_FDT_UP, mode, false);
    dev->timeout_ms = saved;
    if (rc < 0)
        return rc;

    /* The MCU stops answering after a capture unless re-armed like this. */
    if ((rc = gx_command(dev, GX_CMD_NAV, nav, 2, reply, sizeof(reply), &n)) < 0)
        return rc;
    fdt_frame(info, mode, FDT_OP_DOWN, info->fdt_down_delta);
    return gx_command(dev, GX_CMD_FDT_DOWN, mode, 14, NULL, 0, NULL);
}
