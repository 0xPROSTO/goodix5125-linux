/*
 * libusb transport for the standalone tools. The libfprint driver brings its
 * own (GUsb) transport instead.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdlib.h>

#include <libusb.h>

#include "gx5125.h"

#define IFACE_DATA 1
#define EP_OUT 0x01
#define EP_IN  0x81

struct gx_usb {
    libusb_context *ctx;
    libusb_device_handle *h;
};

static int map_err(int rc)
{
    return rc == LIBUSB_ERROR_TIMEOUT ? GX_ERR_TIMEOUT : GX_ERR_IO;
}

static int usb_write(void *ctx, const uint8_t *buf, size_t len, unsigned timeout)
{
    gx_usb *u = ctx;
    int done = 0;
    int rc = libusb_bulk_transfer(u->h, EP_OUT, (unsigned char *)buf, (int)len,
                                  &done, timeout);

    if (rc < 0)
        return map_err(rc);
    return (size_t)done == len ? GX_OK : GX_ERR_IO;
}

static int usb_read(void *ctx, uint8_t *buf, size_t cap, size_t *got,
                    unsigned timeout)
{
    gx_usb *u = ctx;
    int done = 0;
    int rc = libusb_bulk_transfer(u->h, EP_IN, buf, (int)cap, &done, timeout);

    *got = (size_t)done;
    if (rc < 0 && !(rc == LIBUSB_ERROR_TIMEOUT && done > 0))
        return map_err(rc);
    return GX_OK;
}

int gx_usb_open(gx_usb **out, gx_transport *t)
{
    gx_usb *u = calloc(1, sizeof(*u));

    if (!u)
        return GX_ERR_NOMEM;
    if (libusb_init(&u->ctx) < 0)
        goto fail;

    u->h = libusb_open_device_with_vid_pid(u->ctx, GX_VID, GX_PID);
    if (!u->h)
        goto fail;

    libusb_set_auto_detach_kernel_driver(u->h, 1);
    if (libusb_claim_interface(u->h, IFACE_DATA) < 0)
        goto fail;

    t->write = usb_write;
    t->read = usb_read;
    t->ctx = u;
    t->interruptible = false;
    *out = u;
    return GX_OK;

fail:
    gx_usb_close(u);
    return GX_ERR_IO;
}

void gx_usb_close(gx_usb *u)
{
    if (!u)
        return;
    if (u->h) {
        libusb_release_interface(u->h, IFACE_DATA);
        libusb_close(u->h);
    }
    if (u->ctx)
        libusb_exit(u->ctx);
    free(u);
}
