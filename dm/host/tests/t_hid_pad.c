// HID unit test: the vibration devices (hle_hid.c, dm_rumble_attach) and a physical controller's motion in the six-axis state (dm_pad_motion_set).
// hle_hid.c itself is included, so the hooks run as the game calls them; the host services around it are stubbed below. Expected values are hand-computed.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../hle_hid.c"

// ---- stubs for what the rest of the host provides ----
static uint64_t g_now = 1000000000ull, g_guest = 5000000000ull; static int g_opmode = 1;
uint64_t now_ns(void) { return g_now; }
uint64_t guest_ns(void) { return g_guest; }
int dm_opmode(void) { return g_opmode; }
uint64_t dm_guest_frame(void) { return 0; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
void hle_signal_system_event(void *g) { (void)g; }
int touch_fill_n(void *out, int max, uint64_t now) { (void)out; (void)max; (void)now; return 0; }
void dm_touch_set(const dm_touch *t, int n) { (void)t; (void)n; }
uint64_t g_t0_ns;

static int fails, cases;
#define CHECK(what, cond) do { cases++; if (cond) printf("✓ %s\n", what); else { fails++; printf("✗ %s (line %d)\n", what, __LINE__); } } while (0)
static int near(float a, float b) { return fabsf(a - b) < 2e-4f; }

// ---- the rumble sink records every call ----
static struct { int dev; float v[4]; } g_calls[64]; static int g_ncalls;
static void sink(int dev, float al, float fl, float ah, float fh) { if (g_ncalls < 64) { g_calls[g_ncalls].dev = dev; g_calls[g_ncalls].v[0] = al; g_calls[g_ncalls].v[1] = fl; g_calls[g_ncalls].v[2] = ah; g_calls[g_ncalls].v[3] = fh; } g_ncalls++; }
static void send(uint32_t h, float al, float fl, float ah, float fh) { float v[4] = {al, fl, ah, fh}; h_hid_SendVib(&h, v); }

int main(void) {
    const uint32_t id0 = 0, id_hh = 0x20;
    uint32_t hs[4];

    // ---- no sink attached: as before, no vibration device ----
    CHECK("no sink: vibration not permitted", h_hid_VibPermit() == 0);
    CHECK("no sink: 0 handles", h_hid_VibHandles(hs, 2, &id0, STYLE_FULLKEY) == 0);
    send(0x00000000, 1, 160, 1, 320); CHECK("no sink: sending is harmless", g_ncalls == 0);

    // ---- attached, docked (npad 0, full-key style, bit 0) ----
    dm_rumble_attach(sink);
    CHECK("sink: vibration permitted", h_hid_VibPermit() == 1);
    CHECK("full-key: 2 devices", h_hid_VibHandles(hs, 4, &id0, STYLE_FULLKEY) == 2);
    CHECK("handles: bit 0 | npad 0 << 8 | device << 16", hs[0] == 0x00000000u && hs[1] == 0x00010000u);
    CHECK("count 1 asked: 1 handle", h_hid_VibHandles(hs, 1, &id0, STYLE_FULLKEY) == 1 && hs[0] == 0);
    CHECK("count 0 asked: 0 handles", h_hid_VibHandles(hs, 0, &id0, STYLE_FULLKEY) == 0);
    CHECK("a style set that includes the connected one: 2 devices", h_hid_VibHandles(hs, 4, &id0, STYLE_FULLKEY | STYLE_JOYDUAL | STYLE_HANDHELD) == 2 && hs[0] == 0);
    CHECK("joy-con styles only: no device (the controller is full-key)", h_hid_VibHandles(hs, 4, &id0, STYLE_JOYLEFT | STYLE_JOYRIGHT) == 0);
    const uint32_t id1 = 1;
    CHECK("another npad id: no device", h_hid_VibHandles(hs, 4, &id1, STYLE_FULLKEY) == 0);

    uint32_t info[2];
    h_hid_VibInfo(info, &(uint32_t){0x00000000}); CHECK("info left: LRA, position 1", info[0] == 1 && info[1] == 1);
    h_hid_VibInfo(info, &(uint32_t){0x00010000}); CHECK("info right: LRA, position 2", info[0] == 1 && info[1] == 2);
    h_hid_VibInfo(info, &(uint32_t){0x00020000}); CHECK("info device 2: none", info[0] == 0 && info[1] == 0);
    h_hid_VibInfo(info, &(uint32_t){0x00000100}); CHECK("info other npad: none", info[0] == 0 && info[1] == 0);
    h_hid_VibInfo(info, &(uint32_t){0x00000005}); CHECK("info style bit not connected: none", info[0] == 0 && info[1] == 0);

    // ---- values ----
    send(0x00000000, 0.5f, 160, 0.25f, 320);
    CHECK("first value reaches the sink: left", g_ncalls == 1 && g_calls[0].dev == 0 && near(g_calls[0].v[0], 0.5f) && near(g_calls[0].v[1], 160) && near(g_calls[0].v[2], 0.25f) && near(g_calls[0].v[3], 320));
    send(0x00000000, 0.5f, 160, 0.25f, 320); CHECK("the same value again is not forwarded", g_ncalls == 1);
    send(0x00010000, 0.5f, 160, 0.25f, 320); CHECK("the same value on the other device is forwarded (right)", g_ncalls == 2 && g_calls[1].dev == 1);
    send(0x00000000, 0.5f, 161, 0.25f, 320); CHECK("a changed frequency is forwarded", g_ncalls == 3 && near(g_calls[2].v[1], 161));
    send(0x00000000, 3.f, 5000.f, -1.f, 10.f);
    CHECK("clamped: amplitude 3 → 1, 5000 Hz → 1252; amplitude -1 → 0 with frequency 0",
          g_ncalls == 4 && near(g_calls[3].v[0], 1) && near(g_calls[3].v[1], 1252) && near(g_calls[3].v[2], 0) && near(g_calls[3].v[3], 0));
    send(0x00000000, NAN, NAN, 0.5f, NAN);
    CHECK("NaN: amplitude 0 (frequency 0); a sounding band with NaN frequency → 40 Hz",
          g_ncalls == 5 && near(g_calls[4].v[0], 0) && near(g_calls[4].v[1], 0) && near(g_calls[4].v[2], 0.5f) && near(g_calls[4].v[3], 40));
    send(0x00000000, 0.f, 999.f, 0.f, 999.f);
    CHECK("silence: all zeros whatever the frequencies", g_ncalls == 6 && g_calls[5].v[0] == 0 && g_calls[5].v[1] == 0 && g_calls[5].v[2] == 0 && g_calls[5].v[3] == 0);
    send(0x00000000, 0.f, 160.f, 0.f, 320.f); CHECK("silence again (other frequencies): not forwarded", g_ncalls == 6);
    send(0x00020000, 1, 160, 1, 320); send(0x00000100, 1, 160, 1, 320); send(0x00000007, 1, 160, 1, 320);
    CHECK("bad handles (device 2, other npad, style bit 7) are ignored", g_ncalls == 6);

    // actual value: right still holds {0.5, 160, 0.25, 320}; left is silent: default frequencies
    float act[4];
    h_hid_VibActual(act, &(uint32_t){0x00010000}); CHECK("actual (right): what it last got", near(act[0], 0.5f) && near(act[1], 160) && near(act[2], 0.25f) && near(act[3], 320));
    h_hid_VibActual(act, &(uint32_t){0x00000000}); CHECK("actual (left, silent): 0 / 160 / 0 / 320", near(act[0], 0) && near(act[1], 160) && near(act[2], 0) && near(act[3], 320));
    h_hid_VibActual(act, &(uint32_t){0x00020000}); CHECK("actual (bad handle): silence", near(act[0], 0) && near(act[2], 0));

    // batch forms: two values, left then right
    g_ncalls = 0;
    uint32_t bh[2] = {0x00000000, 0x00010000}; float bv[8] = {0.1f, 100, 0.2f, 200, 0.3f, 300, 0.4f, 400};
    CHECK("detail batch returns success", h_hid_SendVibs(bh, bv, 2) == 0);
    CHECK("detail batch: left then right", g_ncalls == 2 && g_calls[0].dev == 0 && near(g_calls[0].v[0], 0.1f) && near(g_calls[0].v[3], 200) && g_calls[1].dev == 1 && near(g_calls[1].v[0], 0.3f) && near(g_calls[1].v[3], 400));
    g_ncalls = 0; float bv2[8] = {0.6f, 100, 0.2f, 200, 0.7f, 300, 0.4f, 400};
    CHECK("public batch returns success", h_hid_SendVibsPub(bh, bv2, 2) == 0 && g_ncalls == 2 && near(g_calls[0].v[0], 0.6f) && near(g_calls[1].v[0], 0.7f));

    // re-attach resets the memory: silence first, so a repeat of the old value reaches the sink again
    g_ncalls = 0; dm_rumble_attach(sink); send(0x00000000, 0.6f, 100, 0.2f, 200);
    CHECK("re-attach: the old value reaches the sink again", g_ncalls == 1);

    // handheld: npad 0x20, style bit 1
    g_opmode = 0; g_ncalls = 0;
    CHECK("handheld: 2 devices on npad 0x20 with style bit 1", h_hid_VibHandles(hs, 4, &id_hh, STYLE_HANDHELD) == 2 && hs[0] == (1u | 0x20u << 8) && hs[1] == (1u | 0x20u << 8 | 1u << 16));
    send(hs[1], 0.9f, 200, 0, 0); CHECK("handheld: a value reaches the right device", g_ncalls == 1 && g_calls[0].dev == 1 && near(g_calls[0].v[0], 0.9f));
    CHECK("handheld: the full-key id is no longer connected", h_hid_VibHandles(hs, 4, &id0, STYLE_FULLKEY) == 0);
    g_opmode = 1;

    // detach
    g_ncalls = 0; dm_rumble_attach(NULL);
    CHECK("detached: not permitted, no handles, nothing forwarded", h_hid_VibPermit() == 0 && h_hid_VibHandles(hs, 4, &id0, STYLE_FULLKEY) == 0 && (send(0, 1, 160, 1, 320), g_ncalls == 0));

    // ---- six-axis: no controller motion = the old synthetic sensor, byte for byte ----
    SixAxisState st[3]; uint32_t six = 0;   // full-key, npad 0, device 0
    CHECK("six-axis handle reads connected", h_hid_SixStates(st, 3, &six) == 3);
    int rest_ok = 1;
    for (int i = 0; i < 3; i++) {
        rest_ok &= st[i].delta_ns == 5000000 && st[i].attributes == 1 && st[i].sampling == (int64_t)(g_guest / 5000000) - i;
        rest_ok &= st[i].acc[0] == 0 && st[i].acc[1] == 0 && st[i].acc[2] == -1 && st[i].gyro[0] == 0 && st[i].gyro[1] == 0 && st[i].gyro[2] == 0;
        rest_ok &= st[i].angle[0] == 0 && st[i].angle[1] == 0 && st[i].angle[2] == 0;
        rest_ok &= st[i].dir[0] == 1 && st[i].dir[1] == 0 && st[i].dir[2] == 0 && st[i].dir[3] == 0 && st[i].dir[4] == 1 && st[i].dir[5] == 0 && st[i].dir[6] == 0 && st[i].dir[7] == 0 && st[i].dir[8] == 1;
    }
    CHECK("no motion: lies flat (0,0,-1), no rotation, identity direction, 5 ms apart, newest first", rest_ok);
    CHECK("no motion: at rest", h_hid_SixRest(&six) == 1);

    // ---- a controller reports motion ----
    const float flat[3] = {0, 0, -1}, still[3] = {0, 0, 0};
    dm_pad_motion_set(flat, still);   // t = 1.0 s: first sample, dt 0
    h_hid_SixStates(st, 1, &six);
    CHECK("first sample: acceleration passes through, identity direction, zero angle", near(st[0].acc[2], -1) && st[0].gyro[2] == 0 && st[0].angle[2] == 0 && near(st[0].dir[0], 1) && near(st[0].dir[4], 1) && near(st[0].dir[8], 1));
    CHECK("controller at rest: at rest", h_hid_SixRest(&six) == 1);

    // half a turn about z at 1 revolution / s (2π rad/s): five samples 0.1 s apart → 0.5 rev, the x axis ends at (-1, 0, 0), the y axis at (0, -1, 0)
    const float yaw[3] = {0, 0, (float)(2 * M_PI)};
    for (int k = 0; k < 5; k++) { g_now += 100000000ull; dm_pad_motion_set(flat, yaw); }
    h_hid_SixStates(st, 1, &six);
    CHECK("half a turn: angle z = 0.5 revolutions", near(st[0].angle[2], 0.5f) && near(st[0].angle[0], 0) && near(st[0].angle[1], 0));
    CHECK("half a turn: angular velocity z = 1 revolution / s", near(st[0].gyro[2], 1.f) && near(st[0].gyro[0], 0) && near(st[0].gyro[1], 0));
    CHECK("half a turn: x axis (-1,0,0), y axis (0,-1,0), z axis (0,0,1)", near(st[0].dir[0], -1) && near(st[0].dir[1], 0) && near(st[0].dir[2], 0) && near(st[0].dir[3], 0) && near(st[0].dir[4], -1) && near(st[0].dir[5], 0) && near(st[0].dir[6], 0) && near(st[0].dir[7], 0) && near(st[0].dir[8], 1));
    CHECK("turning fast: not at rest", h_hid_SixRest(&six) == 0);
    int64_t newest = st[0].sampling; SixAxisState h3[3]; h_hid_SixStates(h3, 3, &six);
    CHECK("history: every entry carries the newest reading, sampling numbers count down", h3[2].sampling == h3[0].sampling - 2 && near(h3[2].angle[2], 0.5f) && newest == h3[0].sampling);

    // a quarter turn about x, then a quarter turn about the controller's own y (which by then points along world z)
    g_now += 1000000000ull; dm_pad_motion_set(flat, still);   // a gap over 0.5 s starts again from the identity
    h_hid_SixStates(st, 1, &six); CHECK("after a long gap the orientation starts again at the identity", near(st[0].dir[0], 1) && near(st[0].dir[4], 1) && near(st[0].angle[2], 0));
    const float qx[3] = {(float)(5 * M_PI), 0, 0}, qy[3] = {0, (float)(5 * M_PI), 0};   // 5π rad/s for 0.1 s = a quarter turn
    g_now += 100000000ull; dm_pad_motion_set(flat, qx);
    g_now += 100000000ull; dm_pad_motion_set(flat, qy);
    h_hid_SixStates(st, 1, &six);
    // after the x quarter turn the axes are x=(1,0,0) y=(0,0,1) z=(0,-1,0); the y rate then turns about world z: x → (0,1,0), y stays (0,0,1), z → (1,0,0)
    CHECK("quarter turn about x then about the carried y: rows (0,1,0) (0,0,1) (1,0,0)",
          near(st[0].dir[0], 0) && near(st[0].dir[1], 1) && near(st[0].dir[2], 0) && near(st[0].dir[3], 0) && near(st[0].dir[4], 0) && near(st[0].dir[5], 1) && near(st[0].dir[6], 1) && near(st[0].dir[7], 0) && near(st[0].dir[8], 0));
    CHECK("angles per axis: 0.25 revolutions about x and y", near(st[0].angle[0], 0.25f) && near(st[0].angle[1], 0.25f) && near(st[0].angle[2], 0));
    float det = st[0].dir[0] * (st[0].dir[4] * st[0].dir[8] - st[0].dir[5] * st[0].dir[7]) - st[0].dir[1] * (st[0].dir[3] * st[0].dir[8] - st[0].dir[5] * st[0].dir[6]) + st[0].dir[2] * (st[0].dir[3] * st[0].dir[7] - st[0].dir[4] * st[0].dir[6]);
    CHECK("the direction stays a rotation (determinant 1)", near(det, 1.f));

    // many small steps keep the rows orthonormal (no drift): 2000 samples 5 ms apart with a tumbling rate
    for (int k = 0; k < 2000; k++) { g_now += 5000000ull; float w[3] = {3.1f, -2.2f, 1.7f}; dm_pad_motion_set(flat, w); }
    h_hid_SixStates(st, 1, &six);
    const float *d = st[0].dir; float n0 = d[0]*d[0]+d[1]*d[1]+d[2]*d[2], n1 = d[3]*d[3]+d[4]*d[4]+d[5]*d[5], n2 = d[6]*d[6]+d[7]*d[7]+d[8]*d[8], p01 = d[0]*d[3]+d[1]*d[4]+d[2]*d[5];
    CHECK("2000 small steps: rows unit length and perpendicular", near(n0, 1) && near(n1, 1) && near(n2, 1) && near(p01, 0));

    // staleness: 0.6 s without a sample → the synthetic sensor again
    g_now += 600000000ull; h_hid_SixStates(st, 1, &six);
    CHECK("0.6 s after the last sample: flat and still again", st[0].acc[2] == -1 && st[0].gyro[0] == 0 && st[0].dir[0] == 1 && h_hid_SixRest(&six) == 1);
    dm_pad_motion_set(flat, still); dm_pad_motion_clear(); h_hid_SixStates(st, 1, &six);
    CHECK("cleared: flat and still", st[0].acc[2] == -1 && st[0].angle[2] == 0);
    float bad[3] = {NAN, INFINITY, 0.5f}; dm_pad_motion_set(bad, bad); h_hid_SixStates(st, 1, &six);
    CHECK("non-finite input is read as 0", near(st[0].acc[0], 0) && near(st[0].acc[1], 0) && near(st[0].acc[2], 0.5f) && near(st[0].gyro[0], 0) && near(st[0].gyro[1], 0));
    dm_pad_motion_clear();

    // the phone-shake path (DM_BTN_SHAKE) is unchanged: at 40 ms into a shake the x acceleration is 3 sin(2π · 6 · 0.04)
    dm_input_set(DM_BTN_SHAKE, 0, 0, 0, 0);   // the shake starts at the guest clock now
    uint64_t t0 = g_guest; g_guest = t0 + 40000000ull; h_hid_SixStates(st, 1, &six);
    float want = (float)(3.0 * sin(2 * M_PI * 6 * (double)((g_guest / 5000000) * 5000000 - t0) / 1e9));
    CHECK("shake with no controller motion: x acceleration 3 sin(phase)", near(st[0].acc[0], want) && st[0].acc[2] == -1);
    CHECK("shaking: not at rest", h_hid_SixRest(&six) == 0);
    const float tilt[3] = {0.1f, 0.2f, -0.9f}; dm_pad_motion_set(tilt, still); h_hid_SixStates(st, 1, &six);
    CHECK("shake on top of controller motion: swing added to its acceleration", near(st[0].acc[0], 0.1f + want) && near(st[0].acc[1], 0.2f) && near(st[0].acc[2], -0.9f));

    printf("%d cases, %d failed\n", cases, fails);
    return fails ? 1 : 0;
}
