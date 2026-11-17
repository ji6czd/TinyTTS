/*
 * tinytts - tiny Japanese formant (Klatt-style) speech synthesizer
 *
 *   build : cc -O2 -o tinytts tts.c -lm
 *   usage : ./tinytts [-s speed] [-p f0_hz] [-o out.wav|-] "text"
 *           text = hiragana / katakana / romaji.   "-o -" writes raw s16le 16kHz mono to stdout
 *
 * text notes
 *   ' (apostrophe) after a mora marks the pitch-accent nucleus : ha'shi (箸) / hashi' (橋)
 *   、 , = short pause   。 . = long pause   ？ ? = rising ending
 *   particles must be written as pronounced (は->わ, へ->え, を->お)
 *
 * pipeline: text -> moras -> segments (formant targets) -> 2.5ms frames -> samples
 * No dictionary, no tables bigger than ~1KB, no heap use during synthesis.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define FS     16000
#define FR     40                         /* samples per parameter frame (2.5 ms) */
#define FRMS   (1000.0f * FR / FS)
#define PI     3.14159265f
#define AMPTR  8.0f                       /* amplitude cross-fade limit (ms)      */

/* ------------------------------------------------------------ parameters */
typedef struct {
    float f1, f2, f3, b1, b2, b3, ff, fb, nz;   /* "slow" params: formants, frication filter, nasal zero */
    float av, ah, fr, nas;                      /* "fast" params: voicing, aspiration, frication, nasal mix */
} Par;
#define NSLOW 9
#define NPAR  13

typedef struct { Par p; float dur, tr, f0; int hard; } Seg;

static Par base(void) {
    Par p = {500, 1500, 2500, 80, 100, 150, 4000, 1500, 1000, 0, 0, 0, 0};
    return p;
}

/* ------------------------------------------------------------ resonators */
typedef struct { float a, b, c, y1, y2; } Res;
static void res_set(Res *r, float f, float bw) {
    if (f > 0.45f * FS) f = 0.45f * FS;
    float c = -expf(-2 * PI * bw / FS);
    float b = 2 * expf(-PI * bw / FS) * cosf(2 * PI * f / FS);
    r->a = 1 - b - c; r->b = b; r->c = c;
}
static inline float res_run(Res *r, float x) {
    float y = r->a * x + r->b * r->y1 + r->c * r->y2;
    r->y2 = r->y1; r->y1 = y; return y;
}

/* ------------------------------------------------------------ synth state */
#define GT 512
static float gt[GT + 1];                  /* glottal flow pulse table */
#define TP 0.45f                          /* opening phase  */
#define TN 0.18f                          /* closing phase  */

typedef struct {
    Seg *seg; int n, cap; float speed;
    int si, fi, nf; Par prev;
    float f0c, phase, dph, P, gprev, drift;
    float sm1[NSLOW], sm2[NSLOW];         /* 2-stage smoothed slow params (articulator inertia) */
    uint32_t rng;
    Res r[5];
    float aA, aB, aC, x1, x2;             /* nasal antiresonator */
    float q0, q1, q2, qa1, qa2, qx1, qx2, qy1, qy2; /* frication biquad */
} Tts;

static void ginit(void) {
    for (int i = 0; i <= GT; i++) {
        float ph = (float)i / GT;
        gt[i] = ph < TP ? 0.5f * (1 - cosf(PI * ph / TP))
              : ph < TP + TN ? cosf(PI * 0.5f * (ph - TP) / TN) : 0;
    }
}
static float rnd(Tts *t) {
    t->rng ^= t->rng << 13; t->rng ^= t->rng >> 17; t->rng ^= t->rng << 5;
    return (int32_t)t->rng * (1.0f / 2147483648.0f);
}

/* produce one 2.5ms frame (FR samples, float -1..1). returns 0 at end of utterance */
static int tts_frame(Tts *t, float *o) {
    if (t->si >= t->n) return 0;
    Seg *s = &t->seg[t->si];
    float u = t->fi * FRMS + FRMS;
    float wa = s->hard ? 1 : fminf(1, u / AMPTR);
    Par c; float *cp = (float *)&c, *pp = (float *)&t->prev, *sp = (float *)&s->p;
    /* formants etc.: two cascaded one-pole chasers -> S-shaped motion, short phones undershoot */
    float ka = 1 - expf(-FRMS / fmaxf(s->tr / 3, 2.0f));
    for (int k = 0; k < NSLOW; k++) {
        t->sm1[k] += (sp[k] - t->sm1[k]) * ka;
        t->sm2[k] += (t->sm1[k] - t->sm2[k]) * ka;
        cp[k] = t->sm2[k];
    }
    for (int k = NSLOW; k < NPAR; k++) cp[k] = pp[k] + (sp[k] - pp[k]) * wa;
    /* pitch smoothing + slow natural drift (~±1%) */
    float tau = s->f0 < t->f0c ? 18.0f : 28.0f;
    t->f0c += (s->f0 - t->f0c) * (1 - expf(-FRMS / tau));
    t->drift += (rnd(t) - t->drift) * 0.03f;
    float f0u = t->f0c * (1 + 0.03f * t->drift);

    res_set(&t->r[0], c.f1, c.b1); res_set(&t->r[1], c.f2, c.b2); res_set(&t->r[2], c.f3, c.b3);
    res_set(&t->r[3], 3300, 250);  res_set(&t->r[4], 4300, 300);
    { /* nasal zero */
        Res z; res_set(&z, c.nz, 150);
        t->aA = 1 / z.a; t->aB = -z.b / z.a; t->aC = -z.c / z.a;
    }
    { /* frication band-pass, 0 dB peak */
        float w0 = 2 * PI * fminf(c.ff, 0.45f * FS) / FS, Q = fmaxf(c.ff / fmaxf(c.fb, 200), 0.5f);
        float al = sinf(w0) / (2 * Q), a0 = 1 + al;
        t->q0 = al / a0; t->qa1 = -2 * cosf(w0) / a0; t->qa2 = (1 - al) / a0;
    }
    float av = c.av, ah = c.ah + 0.03f * c.av, fr = c.fr, nas = c.nas;
    for (int j = 0; j < FR; j++) {
        t->phase += t->dph;
        if (t->phase >= 1) {
            t->phase -= 1;
            t->P = FS / f0u * (1 + 0.004f * rnd(t));         /* tiny jitter */
            t->dph = 1 / t->P;
        }
        float x = t->phase * GT; int i0 = (int)x; float g = gt[i0] + (gt[i0 + 1] - gt[i0]) * (x - i0);
        float d = (g - t->gprev) * t->P * (1 / 10.0f);       /* glottal flow derivative, peak ~1 */
        t->gprev = g;
        float in = av * d * 0.5f + ah * rnd(t) * 0.12f;
        for (int k = 0; k < 5; k++) in = res_run(&t->r[k], in);
        float yn = t->aA * in + t->aB * t->x1 + t->aC * t->x2; t->x2 = t->x1; t->x1 = in;
        in += nas * (yn - in);
        float nz = rnd(t) * fr;                              /* frication */
        float q = t->q0 * (nz - t->qx2) - t->qa1 * t->qy1 - t->qa2 * t->qy2;
        t->qx2 = t->qx1; t->qx1 = nz; t->qy2 = t->qy1; t->qy1 = q;
        float out = in * 0.55f + q * 0.38f;
        o[j] = out > 1 ? 1 : out < -1 ? -1 : out;
    }
    if (++t->fi >= t->nf) {
        t->prev = s->p; t->si++; t->fi = 0;
        if (t->si < t->n) t->nf = (int)(t->seg[t->si].dur / FRMS + 0.5f) ? (int)(t->seg[t->si].dur / FRMS + 0.5f) : 1;
    }
    return 1;
}

/* ------------------------------------------------------------ segments */
static void push(Tts *t, Par p, float dur, float tr, int hard, float f0) {
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 256; t->seg = realloc(t->seg, t->cap * sizeof(Seg)); }
    Seg *s = &t->seg[t->n++]; s->p = p; s->dur = dur / t->speed; s->tr = tr; s->hard = hard; s->f0 = f0;
}
static void silence(Tts *t, float ms, float f0) { push(t, base(), ms, 8, 0, f0); }

static Par mixF(Par a, Par b, float w) {
    float *x = (float *)&a, *y = (float *)&b;
    for (int k = 0; k < NSLOW; k++) x[k] += (y[k] - x[k]) * w;
    return a;
}

static const float VF[5][3] = {{750, 1250, 2500}, {300, 2300, 3000}, {340, 1400, 2300}, {480, 1900, 2500}, {500, 900, 2400}};
static int vidx(char v) { return v == 'a' ? 0 : v == 'i' ? 1 : v == 'u' ? 2 : v == 'e' ? 3 : 4; }
static Par vowelPar(char v) {
    Par p = base(); const float *f = VF[vidx(v)];
    p.f1 = f[0]; p.f2 = f[1]; p.f3 = f[2]; p.av = 1; return p;
}

typedef struct {
    char c[4]; char v;
    int type;            /* 0 normal, 1 moraic N, 2 Q, 3 pause */
    int acc, lng, dev, gem;
    float f0, stretch;
} Mora;

static int vless(const char *c) { return c[0] && strchr("kstcfhp", c[0]); }

static void build_N(Tts *t, Mora *m, Mora *nx) {
    Par p = base(); p.f1 = 260; p.b1 = 100; p.f2 = 1200; p.f3 = 2500; p.av = 0.6f; p.nas = 1; p.nz = 1000;
    char n = (nx && nx->type == 0) ? nx->c[0] : 0;
    if (n == 'p' || n == 'b' || n == 'm') { p.f2 = 1000; p.nz = 900; }
    else if (n == 'k' || n == 'g') { p.f2 = 1300; p.nz = 1200; }
    else if (n && strchr("tdnszcjr", n)) { p.f2 = 1600; p.nz = 1500; }
    push(t, p, 90 * m->stretch, 18, 0, m->f0);
}

/* vowel as two parts: onset, then drift toward the next vowel (never a flat plateau) */
static void pushV(Tts *t, Par V, float vd, float tr, Mora *m, Mora *nx) {
    float f0 = m->f0;
    if (m->dev) { push(t, V, vd, tr, 0, f0); return; }
    f0 *= (m->v == 'i' || m->v == 'u') ? 1.03f : m->v == 'a' ? 0.98f : 1.0f;      /* intrinsic pitch */
    float f0a = f0 * (vless(m->c) ? 1.05f : 1.0f);                                 /* onset after voiceless C */
    Par B = V;
    if (nx && nx->type == 0 && nx->v) B = mixF(V, vowelPar(nx->v), nx->c[0] ? 0.25f : 0.4f);
    B.av = 0.88f;
    push(t, V, vd * 0.45f, tr * 1.6f, 0, f0a);
    push(t, B, vd * 0.55f, 45, 0, f0);
}

static void build_mora(Tts *t, Mora *m, Mora *nx) {
    const char *c = m->c; char v = m->v; float f0 = m->f0;
    Par V = vowelPar(v);
    float vd = (m->lng ? 95 : 75) * m->stretch;
    if (m->dev) { V.av = 0; V.ah = 0.6f; vd = 55; }
    int front = (v == 'i' || v == 'e');
    int pal = (c[1] == 'y');
    int gem = m->gem;

    if (!c[0]) { pushV(t, V, vd, 35, m, nx); return; }

    Par C = base(); C.f1 = 350;
    /* place of articulation -> formant locus */
    switch (c[0]) {
    case 'p': case 'b': case 'm': case 'f': C.f2 = 850; C.f3 = 2200; break;
    case 'k': case 'g': if (front) { C.f2 = 2200; C.f3 = 2900; } else { C.f2 = 1300; C.f3 = 2000; } break;
    default: C.f2 = 1700; C.f3 = 2700; break;
    }
    if (pal || (c[0] == 's' && c[1] == 'h') || (c[0] == 'c') || c[0] == 'j') { C.f2 = 2200; C.f3 = 2900; }

    float closure = 40 + (gem ? 60 : 0);

    if ((c[0] == 't' && c[1] == 's') || c[0] == 'c') {              /* affricates ts / ch */
        int ts = (c[0] == 't');
        Par cl = C; push(t, cl, closure, 10, 0, f0);
        Par fp = C; fp.fr = ts ? 1.0f : 0.8f; fp.ff = ts ? 5600 : 3200; fp.fb = ts ? 2600 : 1800;
        push(t, fp, 70, 4, 1, f0);
        pushV(t, V, vd, 28, m, nx); return;
    }
    if (c[0] == 'j') {                                            /* voiced affricate */
        Par cl = C; cl.f1 = 200; cl.av = 0.2f; push(t, cl, 30, 10, 0, f0);
        Par fp = C; fp.f1 = 250; fp.av = 0.5f; fp.fr = 0.45f; fp.ff = 3200; fp.fb = 1800; push(t, fp, 50, 4, 0, f0);
        pushV(t, V, vd, 28, m, nx); return;
    }
    if (c[0] == 'z') {
        Par fp = C; fp.f1 = 250; fp.av = 0.55f; fp.fr = 0.5f; fp.ff = 5500; fp.fb = 2600; push(t, fp, 65, 10, 0, f0);
        pushV(t, V, vd, 28, m, nx); return;
    }
    if (c[0] == 's') {
        Par fp = C;
        if (c[1] == 'h') { fp.fr = 1.0f; fp.ff = 3000; fp.fb = 1800; push(t, fp, 95 + (gem ? 45 : 0), 10, 0, f0); }
        else { fp.fr = 1.0f; fp.ff = 5800; fp.fb = 2600; push(t, fp, 85 + (gem ? 45 : 0), 10, 0, f0); }
        pushV(t, V, vd, 28, m, nx); return;
    }
    if (c[0] == 'f') {
        Par fp = C; fp.fr = 0.45f; fp.ff = 2800; fp.fb = 3500; push(t, fp, 75 + (gem ? 40 : 0), 10, 0, f0);
        pushV(t, V, vd, 25, m, nx); return;
    }
    if (c[0] == 'h') {
        Par ap = (c[1] == 'y') ? vowelPar('i') : V;
        ap.av = 0; ap.ah = 0.9f; if (c[1] == 'y') { ap.fr = 0.3f; ap.ff = 3500; ap.fb = 2000; }
        push(t, ap, 55, 10, 0, f0);
    } else if (strchr("kgtdpb", c[0])) {                          /* plosives */
        float fc, fbw, burst;
        switch (c[0]) {
        case 'k': case 'g': fc = front ? 3200 : 1800; fbw = 1800; burst = 0.9f; break;
        case 't': case 'd': fc = 4200; fbw = 2500; burst = 0.8f; break;
        default: fc = 1300; fbw = 1500; burst = 0.5f; break;
        }
        int vcd = (c[0] == 'g' || c[0] == 'd' || c[0] == 'b');
        Par cl = C; cl.av = vcd ? 0.18f : 0; if (vcd) cl.f1 = 200;
        push(t, cl, closure, 10, 0, f0);
        Par bu = cl; bu.fr = burst * (vcd ? 0.6f : 1); bu.ff = fc; bu.fb = fbw; push(t, bu, 9, 3, 1, f0);
        if (!vcd) { Par as = mixF(C, V, 0.5f); as.av = 0; as.ah = 0.8f; as.fr = burst * 0.15f; as.ff = fc; as.fb = fbw; push(t, as, 20, 6, 0, f0); }
    } else if (c[0] == 'm' || c[0] == 'n') {
        Par mp = C; mp.f1 = 250; mp.b1 = 100; mp.f2 = (c[0] == 'm') ? 1100 : 1500; mp.f3 = 2500;
        if (pal) { mp.f2 = 1900; mp.f3 = 2800; }
        mp.av = 0.6f; mp.nas = 1; mp.nz = (c[0] == 'm') ? 900 : 1500;
        push(t, mp, 55, 15, 0, f0);
    } else if (c[0] == 'r') {
        Par rp = C; rp.f1 = 350; rp.f2 = pal ? 2000 : 1400; rp.f3 = 2300; rp.av = 0.25f; push(t, rp, 22, 12, 0, f0);
    } else if (c[0] == 'y') {
        Par g = vowelPar('i'); push(t, g, 35, 25, 0, f0);
    } else if (c[0] == 'w') {
        Par g = base(); g.f1 = 330; g.f2 = 800; g.f3 = 2300; g.av = 1; push(t, g, 35, 25, 0, f0);
    }
    if (pal && v != 'i') {                                         /* palatal glide */
        Par g = vowelPar('i'); if (m->dev) { g.av = 0; g.ah = 0.5f; }
        push(t, g, 32, 15, 0, f0);
    }
    pushV(t, V, vd, 28, m, nx);
}

/* ------------------------------------------------------------ front-end */
static const char *HIRA[83] = {   /* U+3041 .. U+3093 */
 "<a","a","<i","i","<u","u","<e","e","<o","o","ka","ga","ki","gi","ku","gu","ke","ge","ko","go",
 "sa","za","shi","ji","su","zu","se","ze","so","zo","ta","da","chi","ji","Q","tsu","zu","te","de","to","do",
 "na","ni","nu","ne","no","ha","ba","pa","hi","bi","pi","fu","bu","pu","he","be","pe","ho","bo","po",
 "ma","mi","mu","me","mo","<ya","ya","<yu","yu","<yo","yo","ra","ri","ru","re","ro","<wa","wa","i","e","o","N"};

static int isv(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

/* UTF-8 text -> romaji-ish string (N,Q,R are special tokens) */
static void kana2rom(const char *s, char *out, size_t cap) {
    size_t o = 0, last = 0;
    #define EMIT(str) do { const char *q_ = (str); last = o; while (*q_ && o + 2 < cap) out[o++] = *q_++; } while (0)
    while (*s) {
        unsigned char b = (unsigned char)*s; unsigned cp; int len;
        if (b < 0x80) { cp = b; len = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; len = 3; }
        else { cp = b & 0x07; len = 4; }
        for (int i = 1; i < len && s[i]; i++) cp = (cp << 6) | (s[i] & 0x3F);
        s += len;
        if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;                 /* katakana -> hiragana */
        if (cp == 0x30FC) { EMIT("R"); continue; }
        if (cp == 0x3001 || cp == 0xFF0C) { EMIT(","); continue; }
        if (cp == 0x3002 || cp == 0xFF01 || cp == 0xFF0E) { EMIT("."); continue; }
        if (cp == 0xFF1F) { EMIT("?"); continue; }
        if (cp >= 0x3041 && cp <= 0x3093) {
            const char *r = HIRA[cp - 0x3041];
            if (r[0] == '<') {
                if (o + 4 >= cap) break;
                if (r[1] == 'y') {                                    /* ゃゅょ */
                    if (o > 0 && out[o - 1] == 'i') o--;
                    int soft = (o >= 2 && ((out[o - 1] == 'h' && (out[o - 2] == 's' || out[o - 2] == 'c')))) || (o >= 1 && out[o - 1] == 'j');
                    if (!soft) out[o++] = 'y';
                    out[o++] = r[2];
                } else if (r[1] == 'w') { out[o++] = 'w'; out[o++] = 'a'; }
                else {                                                /* ぁぃぅぇぉ */
                    if (o > last && isv(out[o - 1])) o--;
                    if (o == last) out[o++] = 'w';
                    out[o++] = r[1];
                }
            } else EMIT(r);
            continue;
        }
        if (cp < 0x80 && o + 2 < cap) { out[o++] = (cp >= 'A' && cp <= 'Z' && cp != 'N' && cp != 'Q' && cp != 'R') ? cp + 32 : cp; last = o - 1; }
    }
    out[o] = 0;
}

static const char *CONS[] = {"sh","ch","ts","ky","gy","ny","hy","my","ry","by","py",
                             "k","g","s","z","t","d","n","h","b","p","m","y","r","w","f","j",0};

static int parse(const char *s, Mora *M, int max) {
    int n = 0;
    #define NEWM(ty) (n < max ? (memset(&M[n], 0, sizeof(Mora)), M[n].type = (ty), M[n].stretch = 1, &M[n++]) : &M[max - 1])
    for (int i = 0; s[i];) {
        char ch = s[i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { i++; continue; }
        if (ch == ',' || ch == '.' || ch == '?' || ch == '!') { Mora *m = NEWM(3); m->v = ch == '!' ? '.' : ch; i++; continue; }
        if (ch == '\'') { if (n && M[n - 1].type != 3) M[n - 1].acc = 1; i++; continue; }
        if (ch == 'N') { NEWM(1); i++; continue; }
        if (ch == 'Q') { NEWM(2); i++; continue; }
        if (ch == 'R') {
            if (n && M[n - 1].type == 0) { Mora *m = NEWM(0); m->v = M[n - 2].v; m->lng = 1; }
            i++; continue;
        }
        if (!isv(ch) && ch == s[i + 1] && ch != 'n') { NEWM(2); i++; continue; }
        if (ch == 't' && s[i + 1] == 'c' && s[i + 2] == 'h') { NEWM(2); i++; continue; }
        if (isv(ch)) { Mora *m = NEWM(0); m->v = ch; i++; continue; }
        int L = 0, k;
        for (k = 0; CONS[k]; k++) { int l = (int)strlen(CONS[k]); if (!strncmp(s + i, CONS[k], l)) { L = l; break; } }
        if (!L) { i++; continue; }
        if (isv(s[i + L])) { Mora *m = NEWM(0); memcpy(m->c, s + i, L); m->v = s[i + L]; i += L + 1; }
        else if (!strcmp(CONS[k], "n")) { NEWM(1); i++; }
        else i += L;
    }
    /* long vowels: ou -> oo, ei -> ee ; gemination flag ; devoicing */
    for (int i = 1; i < n; i++) {
        Mora *m = &M[i], *p = &M[i - 1];
        if (m->type == 0 && !m->c[0] && p->type == 0 && ((p->v == 'o' && m->v == 'u') || (p->v == 'e' && m->v == 'i'))) { m->v = p->v; m->lng = 1; }
        if (p->type == 2) m->gem = 1;
    }
    for (int i = 0; i < n; i++) {
        Mora *m = &M[i], *nx = i + 1 < n ? &M[i + 1] : 0;
        if (m->type || (m->v != 'i' && m->v != 'u') || !vless(m->c) || m->acc) continue;
        if (i && M[i - 1].dev) continue;
        if (nx && nx->type == 0 && vless(nx->c)) m->dev = 1;
        else if (nx && nx->type == 2) m->dev = 1;
        else if ((!nx || nx->type == 3) && m->v == 'u') m->dev = 1;
    }
    return n;
}

/* pitch accent: low-high-(drop) per phrase with declination */
static void prosody(Mora *M, int n, float F0) {
    int i = 0, ph = 0;
    while (i < n) {
        int s = i; while (i < n && M[i].type != 3) i++;
        int e = i, cnt = e - s;
        if (cnt > 0) {
            int k = 0; for (int j = s; j < e; j++) if (M[j].acc) { k = j - s + 1; break; }
            float H0 = F0 * (1 - 0.05f * ph);
            for (int j = s; j < e; j++) {
                int mi = j - s + 1; float pos = (float)(mi - 1) / (cnt > 1 ? cnt - 1 : 1);
                float H = H0 * (1 - 0.15f * pos);
                int hi = (k == 1) ? (mi == 1) : (k == 0) ? (mi > 1) : (mi > 1 && mi <= k);
                M[j].f0 = hi ? H : H * 0.80f;
            }
            char pc = e < n ? M[e].v : '.';
            M[e - 1].stretch = pc == ',' ? 1.15f : 1.3f;
            if (pc == '?') M[e - 1].f0 *= 1.35f; else if (pc == '.') M[e - 1].f0 *= 0.92f;
        }
        if (i < n) { M[i].f0 = F0; i++; }
        ph++;
    }
}

static void tts_build(Tts *t, const char *utf8, float speed, float f0) {
    static char rom[4096]; static Mora M[1024];
    memset(t, 0, sizeof *t); t->speed = speed; t->rng = 2463534242u;
    kana2rom(utf8, rom, sizeof rom);
    int n = parse(rom, M, 1024);
    prosody(M, n, f0);
    float lastf0 = f0;
    silence(t, 60, f0);
    for (int i = 0; i < n; i++) {
        Mora *m = &M[i], *nx = i + 1 < n ? &M[i + 1] : 0;
        if (m->type == 3) { silence(t, m->v == ',' ? 150 : 300, lastf0); continue; }
        lastf0 = m->f0;
        if (m->type == 1) build_N(t, m, nx);
        else if (m->type == 2) { if (!nx || nx->type != 0 || !strchr("kgtdpbsctfh", nx->c[0] ? nx->c[0] : 'x')) silence(t, 60, m->f0); }
        else build_mora(t, m, nx);
    }
    silence(t, 150, lastf0);
    t->prev = base(); memcpy(t->sm1, &t->seg[0].p, sizeof t->sm1); memcpy(t->sm2, &t->seg[0].p, sizeof t->sm2); t->f0c = t->seg[0].f0; t->P = FS / t->f0c; t->dph = 1 / t->P;
    t->nf = (int)(t->seg[0].dur / FRMS + 0.5f); if (t->nf < 1) t->nf = 1;
}

/* ------------------------------------------------------------ main */
static void wav_header(FILE *f, uint32_t nsamp) {
    uint32_t db = nsamp * 2, rate = FS, br = FS * 2, v;
    uint16_t s;
    fwrite("RIFF", 1, 4, f); v = 36 + db; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); s = 1; fwrite(&s, 2, 1, f); fwrite(&s, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); s = 2; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&db, 4, 1, f);
}

int main(int argc, char **argv) {
    float speed = 1, f0 = 120; const char *out = "out.wav"; static char text[4096]; text[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) speed = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) f0 = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else { strncat(text, argv[i], sizeof text - strlen(text) - 2); }
    }
    if (!text[0]) { size_t n = fread(text, 1, sizeof text - 1, stdin); text[n] = 0; }
    ginit();
    static Tts T; tts_build(&T, text, speed, f0);
    int cap = FS * 60, ns = 0; int16_t *buf = malloc(cap * 2); float fr[FR];
    while (ns + FR <= cap && tts_frame(&T, fr))
        for (int j = 0; j < FR; j++) buf[ns++] = (int16_t)(fr[j] * 32000);
    int raw = !strcmp(out, "-");
    FILE *f = raw ? stdout : fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    if (!raw) wav_header(f, ns);
    fwrite(buf, 2, ns, f);
    if (!raw) fclose(f);
    fprintf(stderr, "%d samples (%.2fs)\n", ns, (float)ns / FS);
    return 0;
}
