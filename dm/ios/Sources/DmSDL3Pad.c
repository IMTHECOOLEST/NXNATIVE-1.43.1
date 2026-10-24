// SDL3 gamepad backend (opt-in: DM_PAD_BACKEND=sdl3, and the app built with SDL3, see docs/development.md). Without DM_HAVE_SDL3 this file is two stubs.
//
// A thread polls SDL's gamepad state every 2 ms and reports what changed through dm_ctrl_ops, as DmController.m does for GameController:
//   Buttons  by position (SOUTH / EAST / WEST / NORTH) through dm_pad_face_buttons, so the face layout setting means the same on both backends; the sticks' y axis is flipped (SDL: down is positive).
//   Motion   the gyro and accelerometer of the first gamepad that has both (SDL: rad/s and m/s², frame D of dm_pad_core.h: the force the sensor feels, flat face up = +z).
//   Rumble   SDL has two motors, a low-frequency and a high-frequency one: the Switch's low band drives the first, the high band the second (the stronger of the left and right devices each). SDL_RumbleGamepad
//            takes a duration, so a value that holds is refreshed every 60 ms with a 150 ms duration. SDL's controller rumble has no layered bands per side: for a DualSense, GameController (the default
//            backend) gives the richer haptics.
// SDL's HIDAPI drivers are switched off: on iOS they would go through CoreBluetooth, which needs a usage description the app does not carry; controllers come through SDL's GameController (MFi) driver.
#include "dm_ctrl.h"

#ifndef DM_HAVE_SDL3
int dm_sdl3_available(void) { return 0; }
int dm_sdl3_start(const dm_ctrl_ops *ops) { (void)ops; return 0; }
#else
#include <SDL3/SDL.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <string.h>
#include "dm.h"
#include "dm_input.h"
#include "dm_pad_core.h"

#define MAXPAD 4
typedef struct { SDL_Gamepad *gp; SDL_JoystickID id; dm_ctrl_state last; int has_motion; } pad_t;
static pad_t g_pads[MAXPAD];
static int g_npads, g_active = -1, g_primary = -1;   // g_primary: the pad whose motion and rumble are used
static dm_ctrl_ops g_ops;
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
static float g_rv[2][4]; static int g_rdirty;   // the newest value per device (amp low, freq low, amp high, freq high)

int dm_sdl3_available(void) { return 1; }

static void rumble_sink(int dev, float al, float fl, float ah, float fh) {
    if (dev < 0 || dev > 1) return;
    pthread_mutex_lock(&g_rmu);
    g_rv[dev][0] = al; g_rv[dev][1] = fl; g_rv[dev][2] = ah; g_rv[dev][3] = fh; g_rdirty = 1;
    pthread_mutex_unlock(&g_rmu);
}

static dm_ctrl_state read_state(SDL_Gamepad *g) {
    uint64_t m = dm_pad_face_buttons(dm_ctrl_face_layout(),
        SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH), SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST),
        SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_WEST), SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_NORTH));
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  m |= DM_BTN_L;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) m |= DM_BTN_R;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_STICK))  m |= DM_BTN_LS;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) m |= DM_BTN_RS;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP))    m |= DM_BTN_UP;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  m |= DM_BTN_DOWN;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  m |= DM_BTN_LEFT;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) m |= DM_BTN_RIGHT;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START)) m |= DM_BTN_PLUS;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK))  m |= DM_BTN_MINUS;
    const int TRIG = 8192;   // a quarter of the trigger's travel counts as pressed
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIG)  m |= DM_BTN_ZL;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIG) m |= DM_BTN_ZR;
    #define AX(a) ((float)SDL_GetGamepadAxis(g, a) / 32767.0f)
    dm_ctrl_state s = {m, dm_pad_axis(AX(SDL_GAMEPAD_AXIS_LEFTX)), dm_pad_axis(-AX(SDL_GAMEPAD_AXIS_LEFTY)),
                          dm_pad_axis(AX(SDL_GAMEPAD_AXIS_RIGHTX)), dm_pad_axis(-AX(SDL_GAMEPAD_AXIS_RIGHTY))};
    #undef AX
    return s;
}

static void choose_primary(void) {   // the first pad with gyro and accelerometer, else the first pad
    int best = g_npads ? 0 : -1;
    for (int i = 0; i < g_npads; i++) if (g_pads[i].has_motion) { best = i; break; }
    if (best == g_primary) return;
    if (g_primary >= 0 && g_primary < g_npads && g_pads[g_primary].has_motion) {
        SDL_SetGamepadSensorEnabled(g_pads[g_primary].gp, SDL_SENSOR_GYRO, false); SDL_SetGamepadSensorEnabled(g_pads[g_primary].gp, SDL_SENSOR_ACCEL, false);
    }
    dm_rumble_attach(NULL); dm_ctrl_motion_off();
    g_primary = best;
    if (best < 0) return;
    pad_t *p = &g_pads[best];
    if (p->has_motion && dm_ctrl_motion_enabled()) {
        SDL_SetGamepadSensorEnabled(p->gp, SDL_SENSOR_GYRO, true); SDL_SetGamepadSensorEnabled(p->gp, SDL_SENSOR_ACCEL, true);
    } else p->has_motion = 0;
    int rumble = dm_ctrl_rumble_enabled() && SDL_GetBooleanProperty(SDL_GetGamepadProperties(p->gp), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    if (rumble) { pthread_mutex_lock(&g_rmu); memset(g_rv, 0, sizeof g_rv); g_rdirty = 0; pthread_mutex_unlock(&g_rmu); dm_rumble_attach(rumble_sink); }
    LOG("SDL3 controller %s: motion %s, rumble %s", SDL_GetGamepadName(p->gp) ? SDL_GetGamepadName(p->gp) : "?", p->has_motion ? "on" : "off", rumble ? "on" : "off");
}

static void add_pad(SDL_JoystickID id) {
    for (int i = 0; i < g_npads; i++) if (g_pads[i].id == id) return;   // already open (the startup scan and the added event both report it)
    if (g_npads >= MAXPAD) return;
    SDL_Gamepad *gp = SDL_OpenGamepad(id);
    if (!gp) { LOG("SDL3: cannot open gamepad %u: %s", (unsigned)id, SDL_GetError()); return; }
    pad_t *p = &g_pads[g_npads++]; memset(p, 0, sizeof *p);
    p->gp = gp; p->id = id; p->last = read_state(gp);
    p->has_motion = SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(gp, SDL_SENSOR_ACCEL);
    choose_primary();
    g_ops.presence(1);
}

static void remove_pad(SDL_JoystickID id) {
    int k = -1; for (int i = 0; i < g_npads; i++) if (g_pads[i].id == id) k = i;
    if (k < 0) return;
    SDL_Gamepad *gp = g_pads[k].gp;
    int was_primary = k == g_primary;
    if (was_primary) { dm_rumble_attach(NULL); dm_ctrl_motion_off(); g_primary = -2; }   // -2: nothing chosen, so choose_primary below picks again whatever the index
    for (int i = k; i + 1 < g_npads; i++) g_pads[i] = g_pads[i + 1];
    g_npads--;
    if (g_primary > k) g_primary--;
    if (g_active == k) g_active = -1; else if (g_active > k) g_active--;
    SDL_CloseGamepad(gp);
    if (was_primary) choose_primary();
    if (!g_npads) { dm_ctrl_state z = {0, 0, 0, 0, 0}; g_ops.state(&z); g_ops.presence(0); }
    else if (g_active < 0) { g_ops.state(&g_pads[0].last); }
}

static void pump_motion(void) {
    if (g_primary < 0 || !g_pads[g_primary].has_motion) return;
    SDL_Gamepad *gp = g_pads[g_primary].gp; float g[3], a[3];
    if (!SDL_GetGamepadSensorData(gp, SDL_SENSOR_GYRO, g, 3) || !SDL_GetGamepadSensorData(gp, SDL_SENSOR_ACCEL, a, 3)) return;
    const float G = 9.80665f;
    float f[3] = {a[0] / G, a[1] / G, a[2] / G};
    dm_ctrl_motion(f, g);
}

static void pump_rumble(void) {
    static int was_on; static uint64_t last_ms;
    if (g_primary < 0) return;
    float v[2][4]; int dirty;
    pthread_mutex_lock(&g_rmu); memcpy(v, g_rv, sizeof v); dirty = g_rdirty; g_rdirty = 0; pthread_mutex_unlock(&g_rmu);
    float gain = dm_ctrl_rumble_gain();
    float lo = (v[0][0] > v[1][0] ? v[0][0] : v[1][0]) * gain, hi = (v[0][2] > v[1][2] ? v[0][2] : v[1][2]) * gain;
    uint64_t now = SDL_GetTicks();
    if (lo > 0.f || hi > 0.f) {
        if (dirty || !was_on || now - last_ms >= 60) { SDL_RumbleGamepad(g_pads[g_primary].gp, dm_hd_to_u16(lo), dm_hd_to_u16(hi), 150); last_ms = now; }
        was_on = 1;
    } else if (was_on || dirty) { SDL_RumbleGamepad(g_pads[g_primary].gp, 0, 0, 0); was_on = 0; }
}

static void *sdl_thread(void *arg) {
    (void)arg;
    pthread_setname_np("dm.pad.sdl3");
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) add_pad(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) remove_pad(e.gdevice.which);
        }
        for (int i = 0; i < g_npads; i++) {
            dm_ctrl_state s = read_state(g_pads[i].gp);
            if (memcmp(&s, &g_pads[i].last, sizeof s)) { g_pads[i].last = s; g_active = i; g_ops.state(&s); }
        }
        pump_motion();
        pump_rumble();
        SDL_Delay(2);
    }
    return NULL;
}

int dm_sdl3_start(const dm_ctrl_ops *ops) {
    g_ops = *ops;
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");                       // see the top of the file
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { LOG("SDL3: SDL_Init failed: %s", SDL_GetError()); return 0; }
    int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);   // controllers already connected at launch
    for (int i = 0; ids && i < n; i++) add_pad(ids[i]);
    SDL_free(ids);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_set_qos_class_np(&at, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_t th; int rc = pthread_create(&th, &at, sdl_thread, NULL); pthread_attr_destroy(&at);
    if (rc) { LOG("SDL3: cannot start the polling thread (%d)", rc); SDL_Quit(); return 0; }
    pthread_detach(th);
    return 1;
}
#endif
