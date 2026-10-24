// Physical controllers: the GameController backend, shared settings, motion mapping, and HD rumble through Core Haptics (see dm_ctrl.h for the environment variables).
//
//   Buttons    GCExtendedGamepad, by position, turned into the game's A/B/X/Y by dm_pad_face_buttons (Xbox layout by default).
//   Motion     GCMotion of the primary controller (a DualSense when there is one): gravity + user acceleration (or the total acceleration) and the rotation rate, brought to frame D and then to the Switch's
//              (dm_ctrl_motion → dm_pad_motion_set). GameController reports the gravity direction, as CoreMotion does (flat, face up: z = -1); the sensor feels the opposite force, so it is negated.
//   Rumble     The Switch's vibration value has two bands per side (a low and a high amplitude / frequency) and the game has a left and a right device. Each side of the controller (its left and right handle:
//              on a DualSense the two voice-coil actuators) gets two layered continuous haptic events, one per band, each with its own intensity and sharpness (dm_hd_to_haptic); the haptic engine mixes them.
//              A controller that only exposes one locality gets both sides mixed into one event (dm_hd_mix). iOS gives apps no access to a DualSense's speaker (it is not an audio output over Bluetooth, and
//              GameController's haptics are actuator-only), so the speaker is not driven.
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#include <math.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <string.h>
#include "dm.h"
#include "dm_input.h"
#include "dm_ctrl.h"
#include "dm_pad_core.h"

// ======== shared settings and motion mapping ========
static int g_face = DM_FACE_XBOX, g_motion_on = 1, g_rumble_on = 1, g_motion_log;
static float g_gain = 1.f;
static dm_axmap g_axmap;   // set in dm_ctrl_start
int dm_ctrl_face_layout(void) { return g_face; }
int dm_ctrl_motion_enabled(void) { return g_motion_on; }
int dm_ctrl_rumble_enabled(void) { return g_rumble_on; }
float dm_ctrl_rumble_gain(void) { return g_gain; }

void dm_ctrl_motion(const float force_g[3], const float gyro_rps[3]) {
    float a[3], w[3];
    dm_axmap_apply(&g_axmap, force_g, a);
    dm_axmap_apply(&g_axmap, gyro_rps, w);
    dm_pad_motion_set(a, w);
    if (g_motion_log) {   // twice a second: frame D in, Switch frame out
        static _Atomic uint64_t last; uint64_t t = now_ns(), l = atomic_load(&last);
        if (t - l > 500000000ull && atomic_compare_exchange_strong(&last, &l, t))
            LOG("motion: in (frame D) acc %.3f %.3f %.3f G gyro %.3f %.3f %.3f rad/s  →  out (Switch) acc %.3f %.3f %.3f G gyro %.3f %.3f %.3f rad/s",
                force_g[0], force_g[1], force_g[2], gyro_rps[0], gyro_rps[1], gyro_rps[2], a[0], a[1], a[2], w[0], w[1], w[2]);
    }
}
void dm_ctrl_motion_off(void) { dm_pad_motion_clear(); }

static float env_float(const char *name, float dflt, float lo, float hi) {
    const char *s = getenv(name); if (!s || !*s) return dflt;
    char *end; float v = strtof(s, &end);
    if (end == s || *end || v != v) { LOG("%s=%s is not a number, using %g", name, s, dflt); return dflt; }
    return v < lo ? lo : v > hi ? hi : v;
}

// ======== GameController backend ========
static dm_ctrl_ops g_ops;
static GCController *g_primary;           // the controller whose motion and haptics are used
static dispatch_queue_t g_inq;            // controller callbacks run here, not on the main thread (input latency does not depend on UI work)

static void gc_read(GCExtendedGamepad *g) {
    uint64_t m = dm_pad_face_buttons(g_face, g.buttonA.isPressed, g.buttonB.isPressed, g.buttonX.isPressed, g.buttonY.isPressed);
    if (g.leftShoulder.isPressed)  m |= DM_BTN_L;
    if (g.rightShoulder.isPressed) m |= DM_BTN_R;
    if (g.leftTrigger.isPressed)   m |= DM_BTN_ZL;
    if (g.rightTrigger.isPressed)  m |= DM_BTN_ZR;
    if (g.leftThumbstickButton.isPressed)  m |= DM_BTN_LS;
    if (g.rightThumbstickButton.isPressed) m |= DM_BTN_RS;
    if (g.dpad.up.isPressed)    m |= DM_BTN_UP;
    if (g.dpad.down.isPressed)  m |= DM_BTN_DOWN;
    if (g.dpad.left.isPressed)  m |= DM_BTN_LEFT;
    if (g.dpad.right.isPressed) m |= DM_BTN_RIGHT;
    if (g.buttonMenu.isPressed)    m |= DM_BTN_PLUS;
    if (g.buttonOptions.isPressed) m |= DM_BTN_MINUS;
    dm_ctrl_state s = {m, dm_pad_axis(g.leftThumbstick.xAxis.value), dm_pad_axis(g.leftThumbstick.yAxis.value),
                          dm_pad_axis(g.rightThumbstick.xAxis.value), dm_pad_axis(g.rightThumbstick.yAxis.value)};
    g_ops.state(&s);
}

// ---- HD rumble ----
// One side of the controller: an engine and two layered players (band 0 = the Switch's low band, 1 = the high band)
@interface DmMotor : NSObject {
@public
    CHHapticEngine *engine;
    id<CHHapticAdvancedPatternPlayer> player[2];
    float lastI[2], lastS[2];
    uint64_t gen;   // counts calls; the idle stop only acts if nothing came since it was scheduled
}
@end
@implementation DmMotor @end

static dispatch_queue_t g_hq;                  // all Core Haptics work happens here
static DmMotor *g_motor[2];                    // device 0 = left, 1 = right; both the same object on a controller with one locality (g_mixed)
static BOOL g_mixed;
static os_unfair_lock g_pend_lock = OS_UNFAIR_LOCK_INIT;
static float g_pend[2][4]; static int g_sched[2];   // the newest value per device, and whether a block to apply it is queued

static DmMotor *make_motor(GCDeviceHaptics *h, GCHapticsLocality loc) {
    CHHapticEngine *e = [h createEngineWithLocality:loc];
    if (!e) return nil;
    e.playsHapticsOnly = YES;
    e.autoShutdownEnabled = NO;
    NSError *err = nil;
    if (![e startAndReturnError:&err]) { LOG("haptics %s: engine did not start: %s", loc.UTF8String, err.localizedDescription.UTF8String); return nil; }
    DmMotor *m = [DmMotor new]; m->engine = e;
    __weak DmMotor *wm = m;
    e.resetHandler = ^{   // the engine was reset by the system: restart it, the players are made again on demand
        DmMotor *s = wm; if (!s) return;
        NSError *er = nil; [s->engine startAndReturnError:&er];
        s->player[0] = s->player[1] = nil; s->lastI[0] = s->lastI[1] = 0;
    };
    e.stoppedHandler = ^(CHHapticEngineStoppedReason r) {
        DmMotor *s = wm; if (!s) return;
        s->player[0] = s->player[1] = nil; s->lastI[0] = s->lastI[1] = 0;
        NSError *er = nil; [s->engine startAndReturnError:&er];
    };
    return m;
}

static id<CHHapticAdvancedPatternPlayer> make_player(DmMotor *m) {
    // a looped continuous event at full intensity and sharpness 0.5; the dynamic parameters set the real intensity (a multiplier) and sharpness (an offset)
    CHHapticEvent *ev = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                      parameters:@[[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f],
                                                                   [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f]]
                                                    relativeTime:0 duration:30.0];
    NSError *err = nil;
    CHHapticPattern *p = [[CHHapticPattern alloc] initWithEvents:@[ev] parameters:@[] error:&err];
    if (!p) return nil;
    id<CHHapticAdvancedPatternPlayer> pl = [m->engine createAdvancedPlayerWithPattern:p error:&err];
    if (!pl) return nil;
    pl.loopEnabled = YES;
    if (![pl startAtTime:CHHapticTimeImmediate error:&err]) return nil;
    return pl;
}

static void set_band(DmMotor *m, int b, float amp, float freq) {
    float i, s; dm_hd_to_haptic(fminf(amp * g_gain, 1.f), freq, &i, &s);
    if (i < 0.01f) {   // silent: the player stays but gets intensity 0; the idle stop removes it later
        if (m->player[b] && m->lastI[b] > 0.f) {
            NSError *err = nil;
            [m->player[b] sendParameters:@[[[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:0.f relativeTime:0]] atTime:CHHapticTimeImmediate error:&err];
        }
        m->lastI[b] = 0.f; return;
    }
    if (!m->player[b]) { m->player[b] = make_player(m); m->lastI[b] = -1.f; if (!m->player[b]) return; }
    if (fabsf(i - m->lastI[b]) < 0.015f && fabsf(s - m->lastS[b]) < 0.02f) return;
    NSError *err = nil;
    [m->player[b] sendParameters:@[[[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:i relativeTime:0],
                                   [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl value:s - 0.5f relativeTime:0]]
                          atTime:CHHapticTimeImmediate error:&err];
    m->lastI[b] = i; m->lastS[b] = s;
}

static void stop_motor_players(DmMotor *m) {
    for (int b = 0; b < 2; b++) {
        if (m->player[b]) { NSError *err = nil; [m->player[b] stopAtTime:CHHapticTimeImmediate error:&err]; m->player[b] = nil; }
        m->lastI[b] = 0.f;
    }
}

static void apply(int dev, const float v[4]) {   // on g_hq
    DmMotor *m = g_motor[dev]; if (!m) return;
    set_band(m, 0, v[0], v[1]);
    set_band(m, 1, v[2], v[3]);
    uint64_t gen = ++m->gen;
    if (v[0] <= 0.f && v[2] <= 0.f) {   // silent: stop the players after a second of silence
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), g_hq, ^{ if (m->gen == gen) stop_motor_players(m); });
    }
}

static void rumble_sink(int dev, float al, float fl, float ah, float fh) {   // called on the game's threads
    if (dev < 0 || dev > 1) return;
    os_unfair_lock_lock(&g_pend_lock);
    g_pend[dev][0] = al; g_pend[dev][1] = fl; g_pend[dev][2] = ah; g_pend[dev][3] = fh;
    int go = !g_sched[dev]; g_sched[dev] = 1;
    os_unfair_lock_unlock(&g_pend_lock);
    if (!go) return;   // a block for this device is already queued; it will read the newest value
    dispatch_async(g_hq, ^{
        float v[4];
        os_unfair_lock_lock(&g_pend_lock);
        g_sched[dev] = 0; memcpy(v, g_pend[dev], sizeof v);
        os_unfair_lock_unlock(&g_pend_lock);
        if (g_mixed) {   // both devices drive the one motor: per band, the stronger of the two devices' amplitudes with its frequency
            float w[4]; os_unfair_lock_lock(&g_pend_lock); memcpy(w, g_pend[1 - dev], sizeof w); os_unfair_lock_unlock(&g_pend_lock);
            float mx[4] = {fmaxf(v[0], w[0]), v[0] >= w[0] ? v[1] : w[1], fmaxf(v[2], w[2]), v[2] >= w[2] ? v[3] : w[3]};
            apply(0, mx);
            return;
        }
        apply(dev, v);
    });
}

static void haptics_detach(void) {
    dm_rumble_attach(NULL);
    dispatch_async(g_hq, ^{
        for (int d = 0; d < 2; d++) { if (g_motor[d]) { stop_motor_players(g_motor[d]); } }
        for (int d = 0; d < 2; d++) { if (g_motor[d]) { [g_motor[d]->engine stopWithCompletionHandler:^(NSError *e) { }]; } g_motor[d] = nil; }
        os_unfair_lock_lock(&g_pend_lock); memset(g_pend, 0, sizeof g_pend); g_sched[0] = g_sched[1] = 0; os_unfair_lock_unlock(&g_pend_lock);
    });
}

static void haptics_attach(GCController *c) {
    if (!g_rumble_on || !c.haptics) return;
    GCDeviceHaptics *h = c.haptics;
    NSSet<GCHapticsLocality> *loc = h.supportedLocalities;
    DmMotor *l = nil, *r = nil; BOOL mixed = NO;
    if ([loc containsObject:GCHapticsLocalityLeftHandle] && [loc containsObject:GCHapticsLocalityRightHandle]) {
        l = make_motor(h, GCHapticsLocalityLeftHandle); r = make_motor(h, GCHapticsLocalityRightHandle);
    }
    if (!l || !r) {   // one locality only (or the pair failed): everything on one motor
        l = r = nil;
        GCHapticsLocality one = [loc containsObject:GCHapticsLocalityHandles] ? GCHapticsLocalityHandles : [loc containsObject:GCHapticsLocalityDefault] ? GCHapticsLocalityDefault : [loc containsObject:GCHapticsLocalityAll] ? GCHapticsLocalityAll : nil;
        if (one) { l = make_motor(h, one); r = l; mixed = YES; }
    }
    if (!l || !r) { LOG("controller %s: no usable haptics", c.vendorName.UTF8String); return; }
    dispatch_sync(g_hq, ^{ g_motor[0] = l; g_motor[1] = r; g_mixed = mixed; });
    dm_rumble_attach(rumble_sink);
    LOG("controller %s: HD rumble on (%s, gain %g)", c.vendorName.UTF8String, mixed ? "one motor, both sides mixed" : "left and right handles, two bands layered on each", g_gain);
}

// ---- motion ----
static void motion_attach(GCController *c) {
    if (!g_motion_on || !c.motion) return;
    GCMotion *mo = c.motion;
    if (mo.sensorsRequireManualActivation) mo.sensorsActive = YES;
    mo.valueChangedHandler = ^(GCMotion *m) {
        double a[3];
        if (m.hasGravityAndUserAcceleration) { a[0] = m.gravity.x + m.userAcceleration.x; a[1] = m.gravity.y + m.userAcceleration.y; a[2] = m.gravity.z + m.userAcceleration.z; }
        else { a[0] = m.acceleration.x; a[1] = m.acceleration.y; a[2] = m.acceleration.z; }
        float f[3] = {(float)-a[0], (float)-a[1], (float)-a[2]};   // gravity direction → the force the sensor feels
        float w[3] = {0, 0, 0};
        if (m.hasRotationRate) { w[0] = (float)m.rotationRate.x; w[1] = (float)m.rotationRate.y; w[2] = (float)m.rotationRate.z; }
        dm_ctrl_motion(f, w);
    };
    LOG("controller %s: motion on (%s)", c.vendorName.UTF8String, mo.hasRotationRate ? "gyro + accelerometer" : "accelerometer only");
}
static void motion_detach(GCController *c) {
    if (c.motion) c.motion.valueChangedHandler = nil;
    dm_ctrl_motion_off();
}

// ---- connect / disconnect ----
static int score_of(GCController *c) {   // which controller gets the motion and the haptics: a DualSense first, then whatever has haptics, then motion
    int s = 0;
    if ([c.extendedGamepad isKindOfClass:GCDualSenseGamepad.class]) s += 4;
    if (c.haptics) s += 2;
    if (c.motion) s += 1;
    return s;
}
static void gc_refresh(void) {   // main thread
    int n = 0; GCController *best = nil; int bs = -1;
    for (GCController *c in GCController.controllers) {
        GCExtendedGamepad *g = c.extendedGamepad; if (!g) continue;
        n++;
        c.handlerQueue = g_inq;
        g.valueChangedHandler = ^(GCExtendedGamepad *gp, GCControllerElement *el) { gc_read(gp); };
        int sc = score_of(c); if (sc > bs) { bs = sc; best = c; }
    }
    if (best != g_primary) {
        if (g_primary) { motion_detach(g_primary); haptics_detach(); }
        g_primary = best;
        if (best) {
            if (best.playerIndex == GCControllerPlayerIndexUnset) best.playerIndex = GCControllerPlayerIndex1;
            motion_attach(best); haptics_attach(best);
        }
    }
    if (!n) { dm_ctrl_state z = {0, 0, 0, 0, 0}; g_ops.state(&z); }
    g_ops.presence(n > 0);
}

static void gc_start(void) {
    g_inq = dispatch_queue_create("dm.pad.input", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    g_hq = dispatch_queue_create("dm.pad.haptics", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { gc_refresh(); }];
    [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { gc_refresh(); }];
    gc_refresh();   // controllers already connected at launch
}

// ======== entry ========
void dm_ctrl_start(const dm_ctrl_ops *ops) {
    g_ops = *ops;
    g_face = dm_pad_face_parse(getenv("DM_PAD_FACE"), DM_FACE_XBOX);
    const char *me = getenv("DM_MOTION"), *re = getenv("DM_RUMBLE");
    g_motion_on = !(me && !strcmp(me, "0")); g_rumble_on = !(re && !strcmp(re, "0"));
    g_gain = env_float("DM_RUMBLE_GAIN", 1.f, 0.f, 2.f);
    g_motion_log = getenv("DM_MOTION_LOG") != NULL;
    g_axmap = DM_AXMAP_DEFAULT;
    const char *ax = getenv("DM_MOTION_AXES");
    if (ax && *ax && !dm_axmap_parse(ax, &g_axmap)) LOG("DM_MOTION_AXES=%s is not three of x, y, z with optional signs (such as x,-y,-z); using the default", ax);
    const char *be = getenv("DM_PAD_BACKEND"); if (!be || !*be) be = "auto";
    int want_sdl = !strcmp(be, "sdl3");   // auto is GameController: it drives a DualSense's two actuators with layered bands, which SDL3's two-motor rumble cannot
    if (!strcmp(be, "sdl3") && !dm_sdl3_available()) LOG("DM_PAD_BACKEND=sdl3, but this build has no SDL3 (see docs/development.md, "Controllers"); using GameController");
    else if (strcmp(be, "auto") && strcmp(be, "gc") && strcmp(be, "sdl3")) LOG("DM_PAD_BACKEND=%s is not auto, gc or sdl3; using auto", be);
    LOG("controllers: face keys %s layout, motion %s, rumble %s", g_face == DM_FACE_XBOX ? "Xbox" : "Nintendo", g_motion_on ? "on" : "off", g_rumble_on ? "on" : "off");
    if (want_sdl && dm_sdl3_start(ops)) { LOG("controllers: SDL3 backend"); return; }
    if (want_sdl) LOG("controllers: SDL3 backend did not start; using GameController");
    LOG("controllers: GameController backend");
    gc_start();
}
