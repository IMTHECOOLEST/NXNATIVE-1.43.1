// Controller core unit test (dm/ios/Sources/dm_pad_core.c): face layouts, stick scaling, motion axis maps and the HD rumble mapping.
// Expected values are written by hand from the rules in dm_pad_core.h, not computed by the code under test.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../../ios/Sources/dm_pad_core.h"

static int fails, cases;
#define CHECK(what, cond) do { cases++; if (cond) printf("✓ %s\n", what); else { fails++; printf("✗ %s (line %d)\n", what, __LINE__); } } while (0)
static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

int main(void) {
    const uint64_t A = 1, B = 2, X = 4, Y = 8;   // dm_input.h: DM_BTN_A..Y
    // Xbox layout: bottom A, right B, left X, top Y
    CHECK("xbox: bottom = A", dm_pad_face_buttons(DM_FACE_XBOX, 1, 0, 0, 0) == A);
    CHECK("xbox: right = B", dm_pad_face_buttons(DM_FACE_XBOX, 0, 1, 0, 0) == B);
    CHECK("xbox: left = X", dm_pad_face_buttons(DM_FACE_XBOX, 0, 0, 1, 0) == X);
    CHECK("xbox: top = Y", dm_pad_face_buttons(DM_FACE_XBOX, 0, 0, 0, 1) == Y);
    CHECK("xbox: all four", dm_pad_face_buttons(DM_FACE_XBOX, 1, 1, 1, 1) == (A | B | X | Y));
    CHECK("xbox: none", dm_pad_face_buttons(DM_FACE_XBOX, 0, 0, 0, 0) == 0);
    // Nintendo positions: bottom B, right A, left Y, top X
    CHECK("nintendo: bottom = B", dm_pad_face_buttons(DM_FACE_NINTENDO, 1, 0, 0, 0) == B);
    CHECK("nintendo: right = A", dm_pad_face_buttons(DM_FACE_NINTENDO, 0, 1, 0, 0) == A);
    CHECK("nintendo: left = Y", dm_pad_face_buttons(DM_FACE_NINTENDO, 0, 0, 1, 0) == Y);
    CHECK("nintendo: top = X", dm_pad_face_buttons(DM_FACE_NINTENDO, 0, 0, 0, 1) == X);
    CHECK("nintendo: bottom + right = A + B", dm_pad_face_buttons(DM_FACE_NINTENDO, 1, 1, 0, 0) == (A | B));
    CHECK("parse xbox", dm_pad_face_parse("xbox", DM_FACE_NINTENDO) == DM_FACE_XBOX);
    CHECK("parse XBOX (case)", dm_pad_face_parse("XBOX", DM_FACE_NINTENDO) == DM_FACE_XBOX);
    CHECK("parse nintendo", dm_pad_face_parse("nintendo", DM_FACE_XBOX) == DM_FACE_NINTENDO);
    CHECK("parse switch", dm_pad_face_parse("switch", DM_FACE_XBOX) == DM_FACE_NINTENDO);
    CHECK("parse position", dm_pad_face_parse("position", DM_FACE_XBOX) == DM_FACE_NINTENDO);
    CHECK("parse junk keeps the default", dm_pad_face_parse("banana", DM_FACE_XBOX) == DM_FACE_XBOX && dm_pad_face_parse("banana", DM_FACE_NINTENDO) == DM_FACE_NINTENDO);
    CHECK("parse NULL keeps the default", dm_pad_face_parse(NULL, DM_FACE_NINTENDO) == DM_FACE_NINTENDO);

    // Sticks
    CHECK("axis 1 → 32767", dm_pad_axis(1.f) == 32767);
    CHECK("axis -1 → -32767", dm_pad_axis(-1.f) == -32767);
    CHECK("axis 0", dm_pad_axis(0.f) == 0);
    CHECK("axis 0.5 → 16384 (rounded)", dm_pad_axis(0.5f) == 16384);
    CHECK("axis clamps above 1", dm_pad_axis(7.f) == 32767);
    CHECK("axis clamps below -1", dm_pad_axis(-7.f) == -32767);
    CHECK("axis NaN → 0", dm_pad_axis(NAN) == 0);

    // Motion axis maps. Default "x,-y,-z": a controller flat face up feels (0, 0, +1) G in frame D and must read (0, 0, -1) in the Switch frame
    float in[3] = {0, 0, 1}, out[3];
    dm_axmap_apply(&DM_AXMAP_DEFAULT, in, out);
    CHECK("default map: flat face up (0,0,+1) → (0,0,-1)", near(out[0], 0) && near(out[1], 0) && near(out[2], -1));
    float in2[3] = {0.25f, 0.5f, 0.75f};
    dm_axmap_apply(&DM_AXMAP_DEFAULT, in2, out);
    CHECK("default map: (0.25, 0.5, 0.75) → (0.25, -0.5, -0.75)", near(out[0], 0.25f) && near(out[1], -0.5f) && near(out[2], -0.75f));
    dm_axmap m;
    CHECK("parse \"x,-y,-z\"", dm_axmap_parse("x,-y,-z", &m) && m.src[0] == 0 && m.src[1] == 1 && m.src[2] == 2 && m.sgn[0] == 1 && m.sgn[1] == -1 && m.sgn[2] == -1);
    CHECK("parse \"y, x, -z\" (spaces, swap)", dm_axmap_parse("y, x, -z", &m) && m.src[0] == 1 && m.src[1] == 0 && m.src[2] == 2 && m.sgn[0] == 1 && m.sgn[1] == 1 && m.sgn[2] == -1);
    dm_axmap_apply(&m, in2, out);
    CHECK("swap map applies: (0.25, 0.5, 0.75) → (0.5, 0.25, -0.75)", near(out[0], 0.5f) && near(out[1], 0.25f) && near(out[2], -0.75f));
    CHECK("parse \"+Z,-X,Y\" (case, plus)", dm_axmap_parse("+Z,-X,Y", &m) && m.src[0] == 2 && m.src[1] == 0 && m.src[2] == 1 && m.sgn[0] == 1 && m.sgn[1] == -1 && m.sgn[2] == 1);
    dm_axmap keep = DM_AXMAP_DEFAULT; dm_axmap probe = keep;
    CHECK("reject a repeated axis \"x,x,z\"", !dm_axmap_parse("x,x,z", &probe) && !memcmp(&probe, &keep, sizeof keep));
    CHECK("reject two tokens \"x,y\"", !dm_axmap_parse("x,y", &probe));
    CHECK("reject four tokens \"x,y,z,x\"", !dm_axmap_parse("x,y,z,x", &probe));
    CHECK("reject an empty token \"x,,y,z\"", !dm_axmap_parse("x,,y,z", &probe));
    CHECK("reject a trailing comma \"x,y,z,\"", !dm_axmap_parse("x,y,z,", &probe));
    CHECK("reject junk \"x,y,w\"", !dm_axmap_parse("x,y,w", &probe));
    CHECK("reject \"xy,z,x\"", !dm_axmap_parse("xy,z,x", &probe));
    CHECK("reject double sign \"x,--y,z\"", !dm_axmap_parse("x,--y,z", &probe));
    CHECK("reject NULL / empty", !dm_axmap_parse(NULL, &probe) && !dm_axmap_parse("", &probe));
    float io[3] = {1, 2, 3}; dm_axmap_parse("z,y,-x", &m); dm_axmap_apply(&m, io, io);
    CHECK("apply in place (z,y,-x on 1,2,3 → 3,2,-1)", near(io[0], 3) && near(io[1], 2) && near(io[2], -1));

    // HD rumble. Frequency on a log scale over 40..1252 Hz: 40 → 0, 1252 → 1, the geometric middle sqrt(40*1252) = 223.783 Hz → 0.5
    float i, s;
    dm_hd_to_haptic(0.6f, 40.f, &i, &s);   CHECK("40 Hz: sharpness 0, intensity = amplitude", near(i, 0.6f) && near(s, 0.f));
    dm_hd_to_haptic(1.f, 1252.f, &i, &s);  CHECK("1252 Hz: sharpness 1", near(i, 1.f) && near(s, 1.f));
    dm_hd_to_haptic(1.f, sqrtf(40.f * 1252.f), &i, &s); CHECK("geometric middle: sharpness 0.5", near(s, 0.5f));
    dm_hd_to_haptic(1.f, 10.f, &i, &s);    CHECK("below range clamps to 0", near(s, 0.f));
    dm_hd_to_haptic(1.f, 5000.f, &i, &s);  CHECK("above range clamps to 1", near(s, 1.f));
    dm_hd_to_haptic(2.f, 160.f, &i, &s);   CHECK("amplitude above 1 clamps", near(i, 1.f));
    dm_hd_to_haptic(-1.f, 160.f, &i, &s);  CHECK("negative amplitude → 0", near(i, 0.f));
    dm_hd_to_haptic(NAN, NAN, &i, &s);     CHECK("NaN → 0 / 0", near(i, 0.f) && near(s, 0.f));
    dm_hd_to_haptic(0.5f, 160.f, &i, &s);  // log2(160/40) / log2(1252/40) = 2 / 4.97 = 0.4024
    CHECK("160 Hz: sharpness 0.4024", near(s, 0.4024f) || fabsf(s - 0.4024f) < 2e-3f);
    CHECK("u16: 0 → 0, 1 → 65535, 0.5 → 32768", dm_hd_to_u16(0.f) == 0 && dm_hd_to_u16(1.f) == 65535 && dm_hd_to_u16(0.5f) == 32768);
    CHECK("u16: clamps and NaN", dm_hd_to_u16(3.f) == 65535 && dm_hd_to_u16(-3.f) == 0 && dm_hd_to_u16(NAN) == 0);

    printf("%d cases, %d failed\n", cases, fails);
    return fails ? 1 : 0;
}
