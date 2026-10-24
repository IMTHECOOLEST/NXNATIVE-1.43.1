// Controller core (see dm_pad_core.h): no Apple or SDL headers.
#include "dm_pad_core.h"
#include <math.h>
#include <strings.h>
#include <string.h>

// dm_input.h's DM_BTN_A..Y, repeated as plain bit numbers so this file includes nothing from the host
enum { BIT_A = 0, BIT_B = 1, BIT_X = 2, BIT_Y = 3 };

int dm_pad_face_parse(const char *s, int dflt) {
    if (!s) return dflt;
    if (!strcasecmp(s, "xbox")) return DM_FACE_XBOX;
    if (!strcasecmp(s, "nintendo") || !strcasecmp(s, "switch") || !strcasecmp(s, "position")) return DM_FACE_NINTENDO;
    return dflt;
}

uint64_t dm_pad_face_buttons(int layout, int south, int east, int west, int north) {
    uint64_t m = 0;
    if (layout == DM_FACE_NINTENDO) {   // A right, B bottom, X top, Y left
        if (south) m |= 1ull << BIT_B;
        if (east)  m |= 1ull << BIT_A;
        if (west)  m |= 1ull << BIT_Y;
        if (north) m |= 1ull << BIT_X;
    } else {                            // Xbox: A bottom, B right, X left, Y top
        if (south) m |= 1ull << BIT_A;
        if (east)  m |= 1ull << BIT_B;
        if (west)  m |= 1ull << BIT_X;
        if (north) m |= 1ull << BIT_Y;
    }
    return m;
}

int32_t dm_pad_axis(float v) {
    if (isnan(v)) return 0;
    if (v > 1.f) v = 1.f;
    if (v < -1.f) v = -1.f;
    return (int32_t)lrintf(v * 32767.f);
}

const dm_axmap DM_AXMAP_DEFAULT = {{0, 1, 2}, {1, -1, -1}};

int dm_axmap_parse(const char *s, dm_axmap *out) {
    if (!s || !out) return 0;
    char b[32]; size_t n = 0;
    for (const char *p = s; *p; p++) {   // drop spaces
        if (*p == ' ' || *p == '\t') continue;
        if (n + 1 >= sizeof b) return 0;
        b[n++] = *p;
    }
    b[n] = 0;
    int commas = 0; for (const char *p = b; *p; p++) commas += *p == ',';
    if (commas != 2) return 0;           // strtok_r would skip empty tokens: "x,,y,z" must not pass
    dm_axmap m; int used = 0, k = 0;
    char *save = NULL;
    for (char *tok = strtok_r(b, ",", &save); tok; tok = strtok_r(NULL, ",", &save), k++) {
        if (k >= 3) return 0;
        int sgn = 1; const char *c = tok;
        if (*c == '-') { sgn = -1; c++; } else if (*c == '+') c++;
        int a;
        if (*c == 'x' || *c == 'X') a = 0; else if (*c == 'y' || *c == 'Y') a = 1; else if (*c == 'z' || *c == 'Z') a = 2; else return 0;
        if (c[1]) return 0;              // nothing after the axis letter
        if (used & (1 << a)) return 0;   // each axis once
        used |= 1 << a; m.src[k] = (int8_t)a; m.sgn[k] = (int8_t)sgn;
    }
    if (k != 3) return 0;
    *out = m; return 1;
}

void dm_axmap_apply(const dm_axmap *m, const float in[3], float out[3]) {
    float t[3] = {in[0], in[1], in[2]};   // in and out may be the same array
    for (int i = 0; i < 3; i++) out[i] = (float)m->sgn[i] * t[m->src[i]];
}

#define HD_FMIN 40.0f
#define HD_FMAX 1252.0f
static float clamp01(float v) { return isnan(v) ? 0.f : v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
static float sharp_of(float f) {
    if (isnan(f) || f < HD_FMIN) f = HD_FMIN;
    if (f > HD_FMAX) f = HD_FMAX;
    return clamp01(log2f(f / HD_FMIN) / log2f(HD_FMAX / HD_FMIN));
}

void dm_hd_to_haptic(float amp, float freq_hz, float *intensity, float *sharpness) {
    if (intensity) *intensity = clamp01(amp);
    if (sharpness) *sharpness = sharp_of(freq_hz);
}

uint16_t dm_hd_to_u16(float amp) { return (uint16_t)lrintf(clamp01(amp) * 65535.f); }
