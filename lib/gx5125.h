/*
 * libgx5125 - Goodix 27c6:5125 fingerprint sensor (Huawei MateBook D, NbD-WDH9)
 *
 * The sensor is a Goodix MCU (firmware GF_ST411SEC_APP_*) in front of a
 * 88x80 capacitive array. Commands travel in clear; images come back over a
 * TLS 1.2 PSK channel in which the sensor is the client and the host the
 * server. The PSK is the one the Windows driver provisioned.
 *
 * Copyright (C) 2026 The touchiddriver authors
 * Protocol knowledge from goodix-fp-dump and goodix-5117-driver (LGPL-2.1+).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GX_VID 0x27c6
#define GX_PID 0x5125

/* ---- errors ---------------------------------------------------------- */

enum {
    GX_OK            = 0,
    GX_ERR_IO        = -1,  /* USB transfer failed */
    GX_ERR_TIMEOUT   = -2,
    GX_ERR_PROTOCOL  = -3,  /* malformed or unexpected frame */
    GX_ERR_DEVICE    = -4,  /* sensor answered with a failure status */
    GX_ERR_TLS       = -5,
    GX_ERR_NOMEM     = -6,
    GX_ERR_CANCELLED = -7,
    GX_ERR_ARG       = -8,
    GX_ERR_UNSUPPORTED = -9, /* firmware/chip we do not know */
    GX_ERR_FINGER    = -10, /* a finger rests where none may be */
};

const char *gx_strerror(int err);

/* ---- framing (proto.c) ----------------------------------------------- */

#define GX_FLAGS_MSG 0xa0  /* plain command protocol */
#define GX_FLAGS_TLS 0xb0  /* TLS record(s) */

#define GX_CMD_NOP              0x00
#define GX_CMD_GET_IMAGE        0x20
#define GX_CMD_FDT_DOWN         0x32
#define GX_CMD_FDT_UP           0x34
#define GX_CMD_FDT_MODE         0x36
#define GX_CMD_NAV              0x50
#define GX_CMD_SLEEP            0x60
#define GX_CMD_IDLE             0x70
#define GX_CMD_WRITE_REG        0x80
#define GX_CMD_READ_REG         0x82
#define GX_CMD_UPLOAD_CONFIG    0x90
#define GX_CMD_SCAN_FREQ        0x94
#define GX_CMD_ENABLE_CHIP      0x96
#define GX_CMD_RESET            0xa2
#define GX_CMD_READ_OTP         0xa6
#define GX_CMD_FW_VERSION       0xa8
#define GX_CMD_QUERY_MCU_STATE  0xae
#define GX_CMD_ACK              0xb0
#define GX_CMD_REQUEST_TLS      0xd0
#define GX_CMD_TLS_ESTABLISHED  0xd4
#define GX_CMD_PSK_READ         0xe4

/* Outer "pack": flags(1) len(LE16) sum(1) payload */
size_t gx_pack_encode(uint8_t flags, const uint8_t *payload, size_t len,
                      uint8_t *out, size_t cap);
/* Returns total pack size (4 + len) once complete, 0 if more bytes are
 * needed, or a negative error if the header is corrupt. */
int gx_pack_decode(const uint8_t *buf, size_t len, uint8_t *flags,
                   const uint8_t **payload, size_t *payload_len);

/* Inner "message": cmd(1) len(LE16, payload+1) payload checksum(1).
 * checksum is 0xaa - sum(previous bytes), or the constant 0x88 when the
 * command does not carry one (NOP, and some replies). */
size_t gx_msg_encode(uint8_t cmd, const uint8_t *payload, size_t len,
                     bool checksum, uint8_t *out, size_t cap);
int gx_msg_decode(const uint8_t *buf, size_t len, uint8_t *cmd,
                  const uint8_t **payload, size_t *payload_len);

/* ---- transport (usb.c, or supplied by the libfprint driver) ----------- */

typedef struct {
    /* Both return GX_OK / GX_ERR_*; read returns the bytes of one USB
     * transfer (may be a partial pack). */
    int (*write)(void *ctx, const uint8_t *buf, size_t len, unsigned timeout_ms);
    int (*read)(void *ctx, uint8_t *buf, size_t cap, size_t *got,
                unsigned timeout_ms);
    void *ctx;
    /* read() returns GX_ERR_CANCELLED by itself when the caller is
     * cancelled, and 0 means no timeout. Otherwise reads are cut into short
     * slices to poll gx_dev.cancelled, and a slice that expires mid-transfer
     * may lose what had arrived of it. */
    bool interruptible;
} gx_transport;

typedef struct gx_usb gx_usb;
int  gx_usb_open(gx_usb **out, gx_transport *transport);
void gx_usb_close(gx_usb *usb);

/* ---- device / command layer (device.c) -------------------------------- */

#define GX_RX_MAX 0x8000

typedef struct gx_tls gx_tls;

typedef struct {
    gx_transport t;
    unsigned timeout_ms;

    /* Polled while waiting for a finger; return true to abort. */
    bool (*cancelled)(void *ctx);
    void *cancel_ctx;

    void (*log)(void *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    void *log_ctx;
    bool trace;  /* hex-dump every frame through log() */

    uint8_t rx[GX_RX_MAX];
    size_t rx_len;

    gx_tls *tls;
} gx_dev;

void gx_dev_init(gx_dev *dev, const gx_transport *t);

/* Sends one command; waits for its ACK (except NOP) and, when reply != NULL,
 * for the reply message with the same command code. */
int gx_command(gx_dev *dev, uint8_t cmd, const uint8_t *payload, size_t len,
               uint8_t *reply, size_t reply_cap, size_t *reply_len);
/* Same, but waits reply_timeout_ms (0 = forever) for the reply. The write
 * and the ACK keep dev->timeout_ms: a write must never wait forever. */
int gx_command_wait(gx_dev *dev, uint8_t cmd, const uint8_t *payload, size_t len,
                    uint8_t *reply, size_t reply_cap, size_t *reply_len,
                    unsigned reply_timeout_ms);
int gx_nop(gx_dev *dev);
/* Sends raw TLS records in a 0xb0 pack. */
int gx_send_tls(gx_dev *dev, const uint8_t *records, size_t len);
/* Reads and discards whatever the sensor still has queued. */
void gx_drain(gx_dev *dev);
/* Receives one complete pack into dev->rx. */
int gx_recv_pack(gx_dev *dev, uint8_t *flags, const uint8_t **payload,
                 size_t *len, unsigned timeout_ms);

/* ---- TLS (tls.c) ------------------------------------------------------ */

#define GX_PSK_LEN 32

int  gx_tls_open(gx_dev *dev, const uint8_t psk[GX_PSK_LEN]);
/* Sends TLS_ESTABLISHED so the MCU leaves the handshake state even when the
 * handshake failed; the state otherwise survives a power cycle. */
void gx_tls_close(gx_dev *dev);
/* Reads exactly len bytes of decrypted application data. */
int  gx_tls_read(gx_dev *dev, uint8_t *buf, size_t len);
int  gx_psk_load(const char *path, uint8_t psk[GX_PSK_LEN]);

/* ---- sensor (sensor.c) ------------------------------------------------- */

#define GX_IMG_W        88   /* samples per row the sensor sends */
#define GX_IMG_H        80
#define GX_IMG_PIXELS   (GX_IMG_W * GX_IMG_H)
#define GX_IMG_RAW_LEN  (8 + GX_IMG_PIXELS / 4 * 6 + 5)  /* 10573 */

typedef struct {
    char firmware[64];
    uint8_t psk_hash[32];
    uint8_t otp[64];
    size_t otp_len;
    uint16_t calibration[4];  /* values for regs 0x220/0x236/0x238/0x23a */
    uint8_t fdt_level[6];     /* per-zone FDT levels derived from the MCU */
    bool fdt_level_valid;
    int fdt_down_delta;       /* finger threshold offset below the baseline */
} gx_info;

/* Plain bring-up: identifies the MCU, reads OTP, uploads the MCU config.
 * Read-only as far as persistent state goes. */
int gx_sensor_init(gx_dev *dev, gx_info *info);
/* Full session: init + TLS. */
int gx_sensor_open(gx_dev *dev, gx_info *info, const uint8_t psk[GX_PSK_LEN]);
void gx_sensor_close(gx_dev *dev);
/* For a sensor that stopped answering plain commands: one left mid-TLS
 * handshake does so until it is told the handshake is over, even across
 * power cycles. Harmless otherwise; retry gx_sensor_open() after it. */
void gx_sensor_unstick(gx_dev *dev);

/* Calibrates and captures a no-finger background frame. */
int gx_sensor_background(gx_dev *dev, gx_info *info, uint16_t *pixels);
/* Arms finger detection and blocks until a finger is down (or cancelled,
 * or timeout_ms elapses; 0 = forever). */
int gx_sensor_wait_finger(gx_dev *dev, gx_info *info, unsigned timeout_ms);
int gx_sensor_capture(gx_dev *dev, uint16_t *pixels);
/* Blocks until the finger is lifted, then re-arms detection for the next
 * touch. GX_ERR_TIMEOUT: still down after timeout_ms, re-armed all the same
 * (detection then fires on the resting finger). */
int gx_sensor_wait_lift(gx_dev *dev, gx_info *info, unsigned timeout_ms);

bool gx_otp_valid(const uint8_t *otp, size_t len);
void gx_otp_calibration(const uint8_t *otp, uint16_t cal[4]);

/* ---- image (image.c) --------------------------------------------------- */

#define GX_CROP_W 64          /* columns 64..87 always read zero */
#define GX_CROP_H GX_IMG_H

void gx_image_decode(const uint8_t *raw, uint16_t *pixels);
/* finger - background, cropped to 64x80, contrast-equalised to 8 bits. */
void gx_image_prepare(const uint16_t *finger, const uint16_t *background,
                      uint8_t *out);
/* Rough quality: share of the crop that shows ridge texture (0..1). */
#define GX_RIDGE_SD_MIN 60.0
/* Below this the finger covered only part of the sensor (measured: full
 * touches >= 0.9, partial ones 0.1..0.6). */
#define GX_MIN_COVERAGE 0.75
double gx_image_coverage_raw(const uint16_t *finger, const uint16_t *background);
/* True if the raw frame shows no finger (valid as a background). */
#define GX_EMPTY_MEAN_MIN 2450.0
#define GX_EMPTY_HP_MAX   80.0
bool gx_image_is_empty(const uint16_t *frame);
int gx_pgm_write(const char *path, const uint8_t *img, int w, int h);
int gx_pgm_write16(const char *path, const uint16_t *img, int w, int h);

/* ---- matching (match.c) ------------------------------------------------ */

/* A template is one preprocessed 64x80 capture; an enrolled finger is a
 * handful of them. */
typedef struct {
    uint8_t img[GX_CROP_W * GX_CROP_H];
} gx_template;

typedef struct {
    int kp_a, kp_b;     /* keypoints found in each image */
    int candidates;     /* unambiguous descriptor matches */
    int inliers;        /* matches consistent with one rigid transform */
    int rotation;       /* angle tried that won, degrees */
    double ncc;         /* ridge correlation of the aligned overlap */
    int overlap;        /* pixels both images see once aligned */
    int score;          /* inliers if the overlap confirms them, else 0 */
} gx_match_result;

void gx_match_detail(const gx_template *a, const gx_template *b, gx_match_result *r);
int gx_match(const gx_template *a, const gx_template *b);
/* Probe against an enrolled set: the best score over its views. */
int gx_match_set(const gx_template *tpls, int n, const gx_template *probe);
/* Keypoints found in a capture; few means a poor or partial touch. */
int gx_template_quality(const gx_template *t);
#define GX_MIN_KEYPOINTS 20

/* A keypoint transform is confirmed when the aligned ridges correlate over
 * a real overlap. Measured on 7 sets (your enrolment, 13 fprintd probes, 5
 * fingers x 8): impostors with >= 4 inliers correlate <= 0.25, genuine ones
 * mostly 0.65..0.95. */
#define GX_NCC_MIN 0.55
#define GX_OVERLAP_MIN 600
/* Score needed to accept: genuine 11/11 fprintd probes, 34/40 leave-one-out;
 * impostors 0/192. */
#define GX_MATCH_THRESHOLD 4
