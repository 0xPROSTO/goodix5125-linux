/*
 * seteval - score probes against an enrolled set, as the driver does.
 *
 *   seteval ENROLL_DIR PROBE_DIR...
 *
 * For each probe prints the best single-view score and the mosaic score.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glob.h>
#include <stdio.h>
#include <string.h>

#include "gx5125.h"

static int load_dir(const char *dir, gx_template *out, int max)
{
    char pattern[1024];
    glob_t g;
    int n = 0;

    snprintf(pattern, sizeof(pattern), "%s/finger-*.pgm", dir);
    if (glob(pattern, 0, NULL, &g) != 0)
        return 0;
    for (size_t k = 0; k < g.gl_pathc && n < max; k++) {
        FILE *f = fopen(g.gl_pathv[k], "rb");
        int w, h, mx;
        if (!f)
            continue;
        if (fscanf(f, "P5 %d %d %d", &w, &h, &mx) == 3 && w == GX_CROP_W && h == GX_CROP_H) {
            fgetc(f);
            if (fread(out[n].img, 1, sizeof(out[n].img), f) == sizeof(out[n].img))
                n++;
        }
        fclose(f);
    }
    globfree(&g);
    return n;
}

int main(int argc, char **argv)
{
    static gx_template enrolled[64], probes[256];
    int ne = load_dir(argv[1], enrolled, 64);

    for (int d = 2; d < argc; d++) {
        int np = load_dir(argv[d], probes, 256);
        printf("%-32s single:", argv[d]);
        int single[256], set[256];
        for (int p = 0; p < np; p++) {
            single[p] = 0;
            for (int t = 0; t < ne; t++) {
                int s = gx_match(&enrolled[t], &probes[p]);
                single[p] = s > single[p] ? s : single[p];
            }
            set[p] = gx_match_set(enrolled, ne, &probes[p]);
            printf(" %2d", single[p]);
        }
        printf("\n%-32s mosaic:", "");
        for (int p = 0; p < np; p++)
            printf(" %2d", set[p]);
        printf("\n");
    }
    return 0;
}
