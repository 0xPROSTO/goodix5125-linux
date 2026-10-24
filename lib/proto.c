/*
 * Goodix MCU framing: an outer "pack" carrying either a command message or
 * raw TLS records.
 *
 *   pack:     flags(1) length(LE16) header_sum(1) payload[length]
 *   message:  cmd(1) length(LE16) payload[length-1] checksum(1)
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <string.h>

#include "gx5125.h"

const char *gx_strerror(int err)
{
    switch (err) {
    case GX_OK:              return "ok";
    case GX_ERR_IO:          return "USB I/O error";
    case GX_ERR_TIMEOUT:     return "timeout";
    case GX_ERR_PROTOCOL:    return "protocol error";
    case GX_ERR_DEVICE:      return "sensor reported failure";
    case GX_ERR_TLS:         return "TLS error (wrong PSK?)";
    case GX_ERR_NOMEM:       return "out of memory";
    case GX_ERR_CANCELLED:   return "cancelled";
    case GX_ERR_ARG:         return "invalid argument";
    case GX_ERR_UNSUPPORTED: return "unsupported sensor or firmware";
    default:                 return "unknown error";
    }
}

size_t gx_pack_encode(uint8_t flags, const uint8_t *payload, size_t len,
                      uint8_t *out, size_t cap)
{
    if (len > 0xffff || cap < len + 4)
        return 0;

    out[0] = flags;
    out[1] = (uint8_t)(len & 0xff);
    out[2] = (uint8_t)(len >> 8);
    out[3] = (uint8_t)(out[0] + out[1] + out[2]);
    memcpy(out + 4, payload, len);
    return len + 4;
}

int gx_pack_decode(const uint8_t *buf, size_t len, uint8_t *flags,
                   const uint8_t **payload, size_t *payload_len)
{
    size_t body;

    if (len < 4)
        return 0;
    if ((uint8_t)(buf[0] + buf[1] + buf[2]) != buf[3])
        return GX_ERR_PROTOCOL;

    body = (size_t)buf[1] | (size_t)buf[2] << 8;
    if (len < body + 4)
        return 0;

    if (flags)
        *flags = buf[0];
    if (payload)
        *payload = buf + 4;
    if (payload_len)
        *payload_len = body;
    return (int)(body + 4);
}

size_t gx_msg_encode(uint8_t cmd, const uint8_t *payload, size_t len,
                     bool checksum, uint8_t *out, size_t cap)
{
    uint8_t sum = 0;

    if (len + 1 > 0xffff || cap < len + 4)
        return 0;

    out[0] = cmd;
    out[1] = (uint8_t)((len + 1) & 0xff);
    out[2] = (uint8_t)((len + 1) >> 8);
    if (len)
        memcpy(out + 3, payload, len);

    for (size_t i = 0; i < len + 3; i++)
        sum += out[i];
    out[len + 3] = checksum ? (uint8_t)(0xaa - sum) : 0x88;
    return len + 4;
}

int gx_msg_decode(const uint8_t *buf, size_t len, uint8_t *cmd,
                  const uint8_t **payload, size_t *payload_len)
{
    size_t mlen;
    uint8_t sum = 0;
    uint8_t check;

    if (len < 4)
        return GX_ERR_PROTOCOL;

    mlen = (size_t)buf[1] | (size_t)buf[2] << 8;  /* payload + checksum */
    if (mlen < 1 || len < mlen + 3)
        return GX_ERR_PROTOCOL;

    for (size_t i = 0; i < mlen + 2; i++)
        sum += buf[i];
    check = buf[mlen + 2];
    if (check != (uint8_t)(0xaa - sum) && check != 0x88)
        return GX_ERR_PROTOCOL;

    *cmd = buf[0];
    *payload = buf + 3;
    *payload_len = mlen - 1;
    return GX_OK;
}
