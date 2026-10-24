/*
 * Raw frame decoding and preprocessing.
 *
 * A frame is 8 header bytes, 88x80 12-bit samples packed 6 bytes per 4
 * samples, and a 5-byte trailer. Columns 64..87 carry no data.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gx5125.h"

#define HEAD 8
#define EQ_WINDOW 15

void gx_image_decode(const uint8_t *raw, uint16_t *px)
{
    const uint8_t *c = raw + HEAD;

    for (int i = 0; i < GX_IMG_PIXELS / 4; i++, c += 6) {
        *px++ = (uint16_t)((c[0] & 0x0f) << 8 | c[1]);
        *px++ = (uint16_t)(c[3] << 4 | c[0] >> 4);
        *px++ = (uint16_t)((c[5] & 0x0f) << 8 | c[2]);
        *px++ = (uint16_t)(c[4] << 4 | c[5] >> 4);
    }
}

/* Local mean/variance equalisation over a square window, via integral
 * images so it stays cheap. */
static void equalise(const float *in, uint8_t *out, int w, int h, int win)
{
    double s[(GX_CROP_W + 1) * (GX_CROP_H + 1)];
    double s2[(GX_CROP_W + 1) * (GX_CROP_H + 1)];
    const int W = w + 1, half = win / 2;

    memset(s, 0, sizeof(s));
    memset(s2, 0, sizeof(s2));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double v = in[y * w + x];
            s[(y + 1) * W + x + 1] = v + s[y * W + x + 1] + s[(y + 1) * W + x] - s[y * W + x];
            s2[(y + 1) * W + x + 1] = v * v + s2[y * W + x + 1] + s2[(y + 1) * W + x] - s2[y * W + x];
        }

    for (int y = 0; y < h; y++) {
        int y0 = y - half < 0 ? 0 : y - half, y1 = y + half + 1 > h ? h : y + half + 1;
        for (int x = 0; x < w; x++) {
            int x0 = x - half < 0 ? 0 : x - half, x1 = x + half + 1 > w ? w : x + half + 1;
            double n = (double)(y1 - y0) * (x1 - x0);
            double sum = s[y1 * W + x1] - s[y0 * W + x1] - s[y1 * W + x0] + s[y0 * W + x0];
            double sq = s2[y1 * W + x1] - s2[y0 * W + x1] - s2[y1 * W + x0] + s2[y0 * W + x0];
            double mean = sum / n, var = sq / n - mean * mean;
            double sd = var > 1e-6 ? sqrt(var) : 1e-3;
            double v = 128.0 + (in[y * w + x] - mean) * 48.0 / sd;
            out[y * w + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
}

void gx_image_prepare(const uint16_t *finger, const uint16_t *background,
                      uint8_t *out)
{
    float diff[GX_CROP_W * GX_CROP_H];

    for (int y = 0; y < GX_CROP_H; y++)
        for (int x = 0; x < GX_CROP_W; x++) {
            int i = y * GX_IMG_W + x;
            /* Ridges lower the reading; flip so ridges are bright. */
            diff[y * GX_CROP_W + x] = background
                ? (float)background[i] - (float)finger[i]
                : 4095.0f - (float)finger[i];
        }
    equalise(diff, out, GX_CROP_W, GX_CROP_H, EQ_WINDOW);
}

/* Share of 8x8 blocks whose local contrast looks like ridges. */
double gx_image_coverage_raw(const uint16_t *finger, const uint16_t *background)
{
    int good = 0, total = 0;

    for (int by = 0; by + 8 <= GX_CROP_H; by += 8)
        for (int bx = 0; bx + 8 <= GX_CROP_W; bx += 8) {
            double s = 0, s2 = 0;
            for (int y = by; y < by + 8; y++)
                for (int x = bx; x < bx + 8; x++) {
                    int i = y * GX_IMG_W + x;
                    double v = background ? (double)background[i] - finger[i]
                                          : 4095.0 - finger[i];
                    s += v;
                    s2 += v * v;
                }
            double mean = s / 64, sd = sqrt(s2 / 64 - mean * mean);
            total++;
            if (sd > GX_RIDGE_SD_MIN)
                good++;
        }
    return total ? (double)good / total : 0.0;
}

int gx_pgm_write(const char *path, const uint8_t *img, int w, int h)
{
    FILE *f = fopen(path, "wb");

    if (!f)
        return GX_ERR_IO;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(img, 1, (size_t)w * h, f);
    return fclose(f) ? GX_ERR_IO : GX_OK;
}

int gx_pgm_write16(const char *path, const uint16_t *img, int w, int h)
{
    FILE *f = fopen(path, "wb");

    if (!f)
        return GX_ERR_IO;
    fprintf(f, "P5\n%d %d\n4095\n", w, h);
    for (int i = 0; i < w * h; i++) {
        fputc(img[i] >> 8, f);
        fputc(img[i] & 0xff, f);
    }
    return fclose(f) ? GX_ERR_IO : GX_OK;
}

/* An empty sensor reads ~2780 on average with little fine texture; a finger
 * pulls the mean below ~2100 and roughly doubles the high-pass energy
 * (measured on the 5125). */
bool gx_image_is_empty(const uint16_t *frame)
{
    double sum = 0, hp = 0;
    int n = 0, m = 0;

    for (int y = 0; y < GX_CROP_H; y++)
        for (int x = 0; x < GX_CROP_W; x++) {
            sum += frame[y * GX_IMG_W + x];
            n++;
        }
    for (int y = 2; y < GX_CROP_H - 2; y++)
        for (int x = 2; x < GX_CROP_W - 2; x++) {
            double s = 0;
            for (int j = -2; j <= 2; j++)
                for (int i = -2; i <= 2; i++)
                    s += frame[(y + j) * GX_IMG_W + x + i];
            double d = frame[y * GX_IMG_W + x] - s / 25;
            hp += d * d;
            m++;
        }
    return sum / n >= GX_EMPTY_MEAN_MIN && sqrt(hp / m) <= GX_EMPTY_HP_MAX;
}
