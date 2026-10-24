/*
 * matcheval - score every pair of preprocessed captures.
 *
 *   matcheval DIR...   each DIR holds finger-NN.pgm of one finger
 *
 * Prints genuine (same DIR) and impostor (different DIR) score ranges.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gx5125.h"

#define MAX_IMG 512

static int read_pgm8(const char *path, uint8_t *out)
{
    FILE *f = fopen(path, "rb");
    int w, h, mx;

    if (!f)
        return -1;
    if (fscanf(f, "P5 %d %d %d", &w, &h, &mx) != 3 || w != GX_CROP_W ||
        h != GX_CROP_H || mx != 255) {
        fclose(f);
        return -1;
    }
    fgetc(f);
    size_t n = fread(out, 1, (size_t)w * h, f);
    fclose(f);
    return n == (size_t)w * h ? 0 : -1;
}

typedef struct {
    double min, max, sum;
    int n;
} stat;

static void add(stat *s, double v)
{
    if (!s->n || v < s->min)
        s->min = v;
    if (!s->n || v > s->max)
        s->max = v;
    s->sum += v;
    s->n++;
}

static void show(const char *name, const stat *g, const stat *i)
{
    printf("%-11s genuine %7.3f..%7.3f (mean %7.3f)   impostor %7.3f..%7.3f (mean %7.3f)\n",
           name, g->min, g->max, g->n ? g->sum / g->n : 0, i->min, i->max,
           i->n ? i->sum / i->n : 0);
}

int main(int argc, char **argv)
{
    static gx_template tpl[MAX_IMG];
    static int label[MAX_IMG];
    static char name[MAX_IMG][256];
    int count = 0;

    for (int d = 1; d < argc; d++) {
        char pattern[1024];
        glob_t g;

        snprintf(pattern, sizeof(pattern), "%s/finger-*.pgm", argv[d]);
        if (glob(pattern, 0, NULL, &g) != 0)
            continue;
        for (size_t k = 0; k < g.gl_pathc && count < MAX_IMG; k++) {
            if (read_pgm8(g.gl_pathv[k], tpl[count].img) < 0)
                continue;
            label[count] = d;
            snprintf(name[count], sizeof(name[0]), "%s", g.gl_pathv[k]);
            count++;
        }
        globfree(&g);
    }

    stat gs[2] = { 0 }, is[2] = { 0 };
    clock_t t0 = clock();
    int pairs = 0;

    for (int a = 0; a < count; a++)
        for (int b = 0; b < count; b++) {
            gx_match_result r;
            if (a == b)
                continue;
            gx_match_detail(&tpl[a], &tpl[b], &r);
            pairs++;
            stat *s = label[a] == label[b] ? gs : is;
            add(&s[0], r.inliers);
            add(&s[1], r.candidates);
            if (getenv("VERBOSE"))
                printf("%s %s %-40s %-40s kp %2d/%2d cand %2d inl %2d rot %+3d score %4d\n",
                       label[a] == label[b] ? "G" : "I", "", name[a], name[b],
                       r.kp_a, r.kp_b, r.candidates, r.inliers, r.rotation, r.score);
        }

    double ms = (double)(clock() - t0) * 1000 / CLOCKS_PER_SEC / (pairs ? pairs : 1);
    printf("%d images, %d ordered pairs, %.1f ms per comparison\n", count, pairs, ms);
    show("inliers", &gs[0], &is[0]);
    show("candidates", &gs[1], &is[1]);
    return 0;
}
