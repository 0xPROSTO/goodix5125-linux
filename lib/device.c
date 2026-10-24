/*
 * Command layer: one command at a time, ACK then (optionally) reply.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gx5125.h"

#define USB_PACKET 64
#define POLL_SLICE_MS 200

void gx_dev_init(gx_dev *dev, const gx_transport *t)
{
    memset(dev, 0, sizeof(*dev));
    dev->t = *t;
    dev->timeout_ms = 2000;
}

static void trace(gx_dev *dev, const char *dir, const uint8_t *buf, size_t len)
{
    char line[3 * 48 + 1];
    size_t shown = len < 48 ? len : 48;

    if (!dev->trace || !dev->log)
        return;
    for (size_t i = 0; i < shown; i++)
        snprintf(line + 3 * i, 4, "%02x ", buf[i]);
    line[3 * shown] = '\0';
    dev->log(dev->log_ctx, "%s %5zu | %s%s", dir, len, line,
             len > shown ? "..." : "");
}

static int send_pack(gx_dev *dev, uint8_t flags, const uint8_t *payload,
                     size_t len)
{
    uint8_t frame[GX_RX_MAX];
    size_t n = gx_pack_encode(flags, payload, len, frame, sizeof(frame));

    if (n == 0)
        return GX_ERR_ARG;

    trace(dev, ">>", frame, n);

    /* The MCU reads whole 64-byte packets. */
    if (n % USB_PACKET) {
        size_t padded = n + USB_PACKET - n % USB_PACKET;
        memset(frame + n, 0, padded - n);
        n = padded;
    }
    return dev->t.write(dev->t.ctx, frame, n, dev->timeout_ms);
}

int gx_send_tls(gx_dev *dev, const uint8_t *records, size_t len)
{
    return send_pack(dev, GX_FLAGS_TLS, records, len);
}

/* Drops the zero bytes in front of the next pack. The MCU pads every pack to
 * whole 64-byte USB packets, also when another pack follows in the same
 * transfer, and no pack starts with flags 0. */
static void skip_padding(gx_dev *dev)
{
    size_t zeros = 0;

    while (zeros < dev->rx_len && dev->rx[zeros] == 0)
        zeros++;
    if (zeros) {
        memmove(dev->rx, dev->rx + zeros, dev->rx_len - zeros);
        dev->rx_len -= zeros;
    }
}

int gx_recv_pack(gx_dev *dev, uint8_t *flags, const uint8_t **payload,
                 size_t *len, unsigned timeout_ms)
{
    uint8_t chunk[GX_RX_MAX];
    unsigned waited = 0;
    int total;

    /* Drop the previously returned pack. */
    if (dev->rx_len && (total = gx_pack_decode(dev->rx, dev->rx_len, NULL,
                                               NULL, NULL)) > 0) {
        memmove(dev->rx, dev->rx + total, dev->rx_len - (size_t)total);
        dev->rx_len -= (size_t)total;
        skip_padding(dev);
    }

    for (;;) {
        size_t got = 0;
        unsigned slice;
        int rc;

        total = gx_pack_decode(dev->rx, dev->rx_len, flags, payload, len);
        if (total < 0) {
            dev->rx_len = 0;
            return total;
        }
        if (total > 0) {
            trace(dev, "<<", dev->rx, (size_t)total);
            return GX_OK;
        }

        if (dev->cancelled && dev->cancelled(dev->cancel_ctx))
            return GX_ERR_CANCELLED;

        if (dev->t.interruptible) {
            slice = timeout_ms ? timeout_ms - waited : 0;
        } else {
            slice = timeout_ms ? timeout_ms - waited : POLL_SLICE_MS;
            if (slice > POLL_SLICE_MS)
                slice = POLL_SLICE_MS;
        }

        rc = dev->t.read(dev->t.ctx, chunk, sizeof(chunk), &got, slice);
        if (rc == GX_ERR_TIMEOUT) {
            waited += slice;
            if (timeout_ms && waited >= timeout_ms)
                return GX_ERR_TIMEOUT;
            continue;
        }
        if (rc < 0)
            return rc;

        if (dev->rx_len + got > sizeof(dev->rx))
            return GX_ERR_PROTOCOL;
        memcpy(dev->rx + dev->rx_len, chunk, got);
        dev->rx_len += got;
        skip_padding(dev);
    }
}

/* Waits for a plain message with the given command code. */
static int recv_msg(gx_dev *dev, uint8_t want, const uint8_t **payload,
                    size_t *len, unsigned timeout_ms)
{
    for (int skipped = 0; skipped < 8; skipped++) {
        const uint8_t *body;
        size_t body_len;
        uint8_t flags, cmd;
        int rc = gx_recv_pack(dev, &flags, &body, &body_len, timeout_ms);

        if (rc < 0)
            return rc;
        if (flags != GX_FLAGS_MSG) {
            if (dev->log)
                dev->log(dev->log_ctx, "skipping pack with flags %02x", flags);
            continue;
        }
        rc = gx_msg_decode(body, body_len, &cmd, payload, len);
        if (rc < 0)
            return rc;
        if (cmd == want)
            return GX_OK;
        if (dev->log)
            dev->log(dev->log_ctx, "skipping message %02x while waiting for %02x",
                     cmd, want);
    }
    return GX_ERR_PROTOCOL;
}

int gx_command(gx_dev *dev, uint8_t cmd, const uint8_t *payload, size_t len,
               uint8_t *reply, size_t reply_cap, size_t *reply_len)
{
    uint8_t msg[GX_RX_MAX];
    const uint8_t *body;
    size_t body_len;
    size_t n;
    int rc;

    n = gx_msg_encode(cmd, payload, len, cmd != GX_CMD_NOP, msg, sizeof(msg));
    if (n == 0)
        return GX_ERR_ARG;

    rc = send_pack(dev, GX_FLAGS_MSG, msg, n);
    if (rc < 0)
        return rc;

    /* An ACK for another command is a late one for an earlier command
     * (TLS_ESTABLISHED is not always acknowledged in time): not ours. */
    for (int stray = 0;; stray++) {
        rc = recv_msg(dev, GX_CMD_ACK, &body, &body_len, dev->timeout_ms);
        if (rc < 0)
            return rc;
        if (body_len < 2)
            return GX_ERR_PROTOCOL;
        if (body[0] == cmd)
            break;
        if (stray == 3)
            return GX_ERR_PROTOCOL;
        if (dev->log)
            dev->log(dev->log_ctx, "skipping ACK for %02x while waiting for %02x",
                     body[0], cmd);
    }
    if (!(body[1] & 0x01))
        return GX_ERR_PROTOCOL;

    if (reply == NULL)
        return GX_OK;

    rc = recv_msg(dev, cmd, &body, &body_len, dev->timeout_ms);
    if (rc < 0)
        return rc;
    if (body_len > reply_cap)
        body_len = reply_cap;
    memcpy(reply, body, body_len);
    if (reply_len)
        *reply_len = body_len;
    return GX_OK;
}

int gx_nop(gx_dev *dev)
{
    static const uint8_t zeros[4];
    uint8_t msg[8];
    size_t n = gx_msg_encode(GX_CMD_NOP, zeros, sizeof(zeros), false, msg,
                             sizeof(msg));
    int rc = send_pack(dev, GX_FLAGS_MSG, msg, n);

    if (rc < 0)
        return rc;
    /* Usually unanswered; swallow a stray ACK if there is one. */
    gx_drain(dev);
    return GX_OK;
}

void gx_drain(gx_dev *dev)
{
    uint8_t chunk[GX_RX_MAX];
    size_t got;

    dev->rx_len = 0;
    while (dev->t.read(dev->t.ctx, chunk, sizeof(chunk), &got, 50) == GX_OK)
        trace(dev, "<< (drained)", chunk, got);
}
