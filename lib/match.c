/*
 * Fingerprint comparison for a 64x80 sensor.
 *
 * The sensor sees a few square millimetres of skin. Plain image correlation
 * is not discriminative at that size: two different fingers rotated so their
 * ridges run parallel correlate almost as well as the same finger (measured:
 * impostor NCC up to 0.71, genuine down to 0.45).
 *
 * What differs between fingers is where ridges end, fork, and where the pores
 * sit. So: corner keypoints with patch descriptors, unambiguous mutual
 * matches, then the largest set agreeing on one rigid transform. Different
 * fingers still reach 4-7 such inliers by chance, so the transform is then
 * checked on the whole overlap: once aligned, the same finger's ridges line
 * up (NCC ~0.65-0.95), a chance transform's do not (<= 0.25). The score is
 * the inlier count of a confirmed transform.
 *
 * Fingers come back rotated. Normalising each patch to its own ridge
 * direction makes every straight ridge look alike and kills the descriptor
 * (measured: genuine mean 8.3 -> 2.5 inliers), so instead the probe's
 * patches are sampled at a few global angles and the transform is required
 * to agree with the angle tried.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gx5125.h"

#define W GX_CROP_W
#define H GX_CROP_H
#define N (W * H)

#define KP_MAX 48
#ifndef RADIUS
#define RADIUS 7             /* descriptor disc radius */
#endif
#define KP_MARGIN (RADIUS + 2)
#ifndef DESC_MIN
#define DESC_MIN 0.70        /* patch NCC to be a candidate */
#endif
#ifndef DESC_GAP
#define DESC_GAP 0.04        /* best must beat second best by this much */
#endif
#ifndef RANSAC_TOL
#define RANSAC_TOL 3.0       /* pixels */
#endif
#ifndef ROT_MAX
#define ROT_MAX 60           /* largest finger rotation tried, degrees */
#endif
#ifndef ROT_STEP
#define ROT_STEP 12
#endif
#ifndef ROT_TOL
#define ROT_TOL 9.0          /* transform rotation vs. the angle tried, degrees */
#endif

#define DEG (M_PI / 180)
#define DESC_MAX ((2 * RADIUS + 1) * (2 * RADIUS + 1))
#define N_ROT (2 * ROT_MAX / ROT_STEP + 1)   /* probe angles tried */

/* Disc sample offsets. */
static int disc_n;
static signed char disc_x[DESC_MAX], disc_y[DESC_MAX];

static void disc_init(void)
{
    int n = 0;

    if (__atomic_load_n(&disc_n, __ATOMIC_ACQUIRE))
        return;
    for (int j = -RADIUS; j <= RADIUS; j++)
        for (int i = -RADIUS; i <= RADIUS; i++)
            if (i * i + j * j <= RADIUS * RADIUS) {
                disc_x[n] = (signed char)i;
                disc_y[n] = (signed char)j;
                n++;
            }
    __atomic_store_n(&disc_n, n, __ATOMIC_RELEASE);
}

typedef struct {
    float x, y;
    float d[DESC_MAX];       /* zero-mean, unit-norm disc samples */
} keypoint;

typedef struct {
    int n;
    keypoint k[KP_MAX];
    float sm[N];             /* smoothed image the patches come from */
} kpset;

/* One comparison's working set, too big for the stack. The probe is
 * described once at every angle and then compared with any number of views. */
typedef struct {
    kpset view;                    /* described at 0 */
    kpset probe;                   /* keypoints and smoothed image */
    keypoint rot[N_ROT][KP_MAX];   /* probe descriptors per angle tried */
} match_ctx;

static float sample(const float *img, float x, float y)
{
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;

    if (x0 < 0) { x0 = 0; fx = 0; }
    if (y0 < 0) { y0 = 0; fy = 0; }
    if (x0 >= W - 1) { x0 = W - 2; fx = 1; }
    if (y0 >= H - 1) { y0 = H - 2; fy = 1; }
    const float *p = img + y0 * W + x0;
    return (1 - fy) * ((1 - fx) * p[0] + fx * p[1]) + fy * ((1 - fx) * p[W] + fx * p[W + 1]);
}

/* Shi-Tomasi corners (ridge endings, forks, pores - not straight ridges),
 * which describe() then samples patches around. */
static void detect(const uint8_t *img, kpset *out)
{
    static _Thread_local float sm[N], gx[N], gy[N], resp[N];

    disc_init();

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float acc = 0, wsum = 0;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++) {
                    int xx = x + i, yy = y + j;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H)
                        continue;
                    float w = (i ? 1.0f : 2.0f) * (j ? 1.0f : 2.0f);
                    acc += w * img[yy * W + xx];
                    wsum += w;
                }
            sm[y * W + x] = acc / wsum;
        }

    memset(gx, 0, sizeof(gx));
    memset(gy, 0, sizeof(gy));
    for (int y = 1; y < H - 1; y++)
        for (int x = 1; x < W - 1; x++) {
            const float *p = sm + y * W + x;
            gx[y * W + x] = (p[-W + 1] + 2 * p[1] + p[W + 1]) - (p[-W - 1] + 2 * p[-1] + p[W - 1]);
            gy[y * W + x] = (p[W - 1] + 2 * p[W] + p[W + 1]) - (p[-W - 1] + 2 * p[-W] + p[-W + 1]);
        }

    float max_resp = 0;
    memset(resp, 0, sizeof(resp));
    for (int y = KP_MARGIN; y < H - KP_MARGIN; y++)
        for (int x = KP_MARGIN; x < W - KP_MARGIN; x++) {
            double a = 0, b = 0, c = 0;
            for (int j = -2; j <= 2; j++)
                for (int i = -2; i <= 2; i++) {
                    int k = (y + j) * W + x + i;
                    a += gx[k] * gx[k];
                    b += gx[k] * gy[k];
                    c += gy[k] * gy[k];
                }
            double half = (a + c) / 2, diff = (a - c) / 2;
            float r = (float)(half - sqrt(diff * diff + b * b));
            resp[y * W + x] = r;
            if (r > max_resp)
                max_resp = r;
        }

    struct { float r; int x, y; } cand[N];
    int nc = 0;
    for (int y = KP_MARGIN; y < H - KP_MARGIN; y++)
        for (int x = KP_MARGIN; x < W - KP_MARGIN; x++) {
            float r = resp[y * W + x];
            if (r < max_resp * 0.05f)
                continue;
            bool peak = true;
            for (int j = -2; j <= 2 && peak; j++)
                for (int i = -2; i <= 2; i++)
                    if ((i || j) && resp[(y + j) * W + x + i] > r) {
                        peak = false;
                        break;
                    }
            if (peak) {
                cand[nc].r = r;
                cand[nc].x = x;
                cand[nc].y = y;
                nc++;
            }
        }
    for (int i = 1; i < nc; i++)
        for (int j = i; j > 0 && cand[j].r > cand[j - 1].r; j--) {
            __typeof__(cand[0]) t = cand[j];
            cand[j] = cand[j - 1];
            cand[j - 1] = t;
        }

    out->n = 0;
    for (int c = 0; c < nc && out->n < KP_MAX; c++) {
        out->k[out->n].x = (float)cand[c].x;
        out->k[out->n].y = (float)cand[c].y;
        out->n++;
    }
    memcpy(out->sm, sm, sizeof(sm));
}

/* (Re)computes every descriptor with the disc rotated by `angle`. */
static void describe(kpset *ks, double angle)
{
    float cs = (float)cos(angle), sn = (float)sin(angle);

    for (int i = 0; i < ks->n; i++) {
        keypoint *k = &ks->k[i];
        double mean = 0, norm = 0;

        for (int s = 0; s < disc_n; s++) {
            k->d[s] = sample(ks->sm, k->x + cs * disc_x[s] - sn * disc_y[s],
                             k->y + sn * disc_x[s] + cs * disc_y[s]);
            mean += k->d[s];
        }
        mean /= disc_n;
        for (int s = 0; s < disc_n; s++) {
            k->d[s] -= (float)mean;
            norm += k->d[s] * k->d[s];
        }
        norm = norm > 1e-6 ? 1 / sqrt(norm) : 0;
        for (int s = 0; s < disc_n; s++)
            k->d[s] *= (float)norm;
    }
}

static float dot(const float *a, const float *b)
{
    float s = 0;

    for (int i = 0; i < disc_n; i++)
        s += a[i] * b[i];
    return s;
}

static double wrap(double a)
{
    while (a > M_PI)
        a -= 2 * M_PI;
    while (a <= -M_PI)
        a += 2 * M_PI;
    return a;
}

/* Rigid transform taking probe coordinates to target coordinates. */
typedef struct {
    double rot, c, s, tx, ty;
} xform;

static void xform_apply(const xform *t, double x, double y, double *ox, double *oy)
{
    *ox = t->c * x - t->s * y + t->tx;
    *oy = t->s * x + t->c * y + t->ty;
}

static xform xform_make(double rot, double tx, double ty)
{
    xform t = { rot, cos(rot), sin(rot), tx, ty };
    return t;
}

/* One tentative correspondence: probe keypoint pb at (bx, by) and a target
 * point (ax, ay); phi is the rotation it implies (from the sampling angle). */
typedef struct {
    int pb;
    float bx, by, ax, ay;
    double phi;
} pair;

#define PAIRS_MAX (KP_MAX * 16)

/* Mutual, unambiguous descriptor matches of the nb probe keypoints kb
 * (sampled at `angle`) in a. */
static int candidates(const kpset *ka, const keypoint *kb, int nb, double angle,
                      pair *out)
{
    static _Thread_local float sim[KP_MAX][KP_MAX];
    int nm = 0;

    for (int j = 0; j < nb; j++)
        for (int i = 0; i < ka->n; i++)
            sim[j][i] = dot(kb[j].d, ka->k[i].d);

    for (int j = 0; j < nb; j++) {
        int bi = -1;
        float best = -1, second = -1;
        for (int i = 0; i < ka->n; i++) {
            if (sim[j][i] > best) {
                second = best;
                best = sim[j][i];
                bi = i;
            } else if (sim[j][i] > second) {
                second = sim[j][i];
            }
        }
        if (bi < 0 || best < DESC_MIN || best - second < DESC_GAP)
            continue;
        bool mutual = true;
        for (int jj = 0; jj < nb; jj++)
            if (jj != j && sim[jj][bi] > best) {
                mutual = false;
                break;
            }
        if (!mutual)
            continue;
        out[nm].pb = j;
        out[nm].bx = kb[j].x;
        out[nm].by = kb[j].y;
        out[nm].ax = ka->k[bi].x;
        out[nm].ay = ka->k[bi].y;
        out[nm].phi = -angle;
        nm++;
    }
    return nm;
}

/* Largest set of pairs agreeing on one rigid transform, in position and in
 * the rotation each pair implies. Counts distinct probe keypoints, so a point
 * seen in several overlapping views is counted once. */
static int consensus(const pair *m, int nm, xform *best_x)
{
    int best_inl = 0;
    bool seen[KP_MAX];

    for (int p = 0; p < nm; p++)
        for (int q = p + 1; q < nm; q++) {
            if (m[p].pb == m[q].pb)
                continue;
            double vax = m[q].ax - m[p].ax, vay = m[q].ay - m[p].ay;
            double vbx = m[q].bx - m[p].bx, vby = m[q].by - m[p].by;
            double la = hypot(vax, vay), lb = hypot(vbx, vby);

            if (la < 6 || lb < 6 || fabs(la - lb) > RANSAC_TOL)
                continue;
            double rot = wrap(atan2(vay, vax) - atan2(vby, vbx));
            if (fabs(wrap(rot - m[p].phi)) > ROT_TOL * DEG ||
                fabs(wrap(rot - m[q].phi)) > ROT_TOL * DEG)
                continue;
            double c = cos(rot), s = sin(rot);
            xform t = xform_make(rot, m[p].ax - (c * m[p].bx - s * m[p].by),
                                 m[p].ay - (s * m[p].bx + c * m[p].by));

            int inl = 0;
            memset(seen, 0, sizeof(seen));
            for (int k = 0; k < nm; k++) {
                double x, y;
                if (seen[m[k].pb] || fabs(wrap(rot - m[k].phi)) > ROT_TOL * DEG)
                    continue;
                xform_apply(&t, m[k].bx, m[k].by, &x, &y);
                if (hypot(x - m[k].ax, y - m[k].ay) <= RANSAC_TOL) {
                    seen[m[k].pb] = true;
                    inl++;
                }
            }
            if (inl > best_inl) {
                best_inl = inl;
                if (best_x)
                    *best_x = t;
            }
        }
    return best_inl;
}

/* Probe (described at every angle in c->rot) against the view (described
 * at 0): tries every angle, keeps the best. The winning pairs are copied to
 * `pairs` when given. */
static int match_sets(const match_ctx *c, gx_match_result *r, xform *x,
                      pair *pairs, int *npairs)
{
    static _Thread_local pair m[PAIRS_MAX];

    r->inliers = 0;
    r->candidates = 0;
    if (npairs)
        *npairs = 0;
    for (int a = 0; a < N_ROT; a++) {
        int deg = -ROT_MAX + a * ROT_STEP;
        xform t;
        int nm, inl;

        nm = candidates(&c->view, c->rot[a], c->probe.n, deg * DEG, m);
        inl = consensus(m, nm, &t);
        if (inl > r->inliers || (inl == r->inliers && nm > r->candidates)) {
            r->inliers = inl;
            r->candidates = nm;
            r->rotation = deg;
            if (x)
                *x = t;
            if (pairs) {
                memcpy(pairs, m, (size_t)nm * sizeof(*m));
                *npairs = nm;
            }
        }
    }
    return r->inliers;
}

/* NCC of b mapped into a's frame by t (b -> a), over the pixels both see. */
static double aligned_ncc(const float *a, const float *b, const xform *t, int *overlap)
{
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    int n = 0;

    for (int y = 2; y < H - 2; y++)
        for (int x = 2; x < W - 2; x++) {
            double dx = x - t->tx, dy = y - t->ty;
            double bx = t->c * dx + t->s * dy, by = -t->s * dx + t->c * dy;

            if (bx < 2 || by < 2 || bx > W - 3 || by > H - 3)
                continue;
            double va = a[y * W + x], vb = sample(b, (float)bx, (float)by);
            sa += va;
            sb += vb;
            saa += va * va;
            sbb += vb * vb;
            sab += va * vb;
            n++;
        }
    *overlap = n;
    if (n < 50)
        return 0;
    double cov = sab - sa * sb / n, var = (saa - sa * sa / n) * (sbb - sb * sb / n);
    return var > 1e-9 ? cov / sqrt(var) : 0;
}

static void prepare_probe(match_ctx *c, const gx_template *probe)
{
    detect(probe->img, &c->probe);
    for (int a = 0; a < N_ROT; a++) {
        describe(&c->probe, (-ROT_MAX + a * ROT_STEP) * DEG);
        memcpy(c->rot[a], c->probe.k, (size_t)c->probe.n * sizeof(keypoint));
    }
}

/* The prepared probe against one enrolled view. */
static void compare(match_ctx *c, const gx_template *view, gx_match_result *r)
{
    xform t = xform_make(0, 0, 0);

    memset(r, 0, sizeof(*r));
    detect(view->img, &c->view);
    describe(&c->view, 0);
    r->kp_a = c->view.n;
    r->kp_b = c->probe.n;
    match_sets(c, r, &t, NULL, NULL);
    r->score = 0;
    if (r->inliers < 3)
        return;
    /* A few chance inliers between different fingers are common; their
     * transform does not line the ridges up. */
    r->ncc = aligned_ncc(c->view.sm, c->probe.sm, &t, &r->overlap);
    if (r->ncc >= GX_NCC_MIN && r->overlap >= GX_OVERLAP_MIN)
        r->score = r->inliers;
}

/* Out of memory scores 0: never a match. */
void gx_match_detail(const gx_template *ta, const gx_template *tb,
                     gx_match_result *r)
{
    match_ctx *c = malloc(sizeof(*c));

    memset(r, 0, sizeof(*r));
    if (!c)
        return;
    prepare_probe(c, tb);
    compare(c, ta, r);
    free(c);
}

int gx_match(const gx_template *a, const gx_template *b)
{
    gx_match_result r;

    gx_match_detail(a, b, &r);
    return r.score;
}

int gx_match_set(const gx_template *tpls, int n, const gx_template *probe)
{
    match_ctx *c = malloc(sizeof(*c));
    int best = 0;

    if (!c)
        return 0;
    prepare_probe(c, probe);
    for (int t = 0; t < n; t++) {
        gx_match_result r;

        compare(c, &tpls[t], &r);
        if (r.score > best)
            best = r.score;
    }
    free(c);
    return best;
}

int gx_template_quality(const gx_template *t)
{
    static _Thread_local kpset k;

    detect(t->img, &k);
    return k.n;
}
