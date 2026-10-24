// Physical controllers on iOS: one of two backends reads them (DmController.m on GameController, the default; DmSDL3Pad.c on SDL3, when the app was built with it), and both report to DmGamepad.m through
// these calls. Buttons and sticks go out as the host's input bits (dm_input.h); motion and rumble go straight to the host (dm_pad_motion_set, dm_rumble_attach).
//
// Environment (Documents/env.txt):
//   DM_PAD_BACKEND=auto|gc|sdl3   auto (the default) and gc: GameController. sdl3: SDL3, when the app was built with it (else GameController, with a log line)
//   DM_PAD_FACE=xbox|nintendo     the face keys' meaning, see dm_pad_core.h (xbox by default)
//   DM_MOTION=0                   leave a controller's gyro and accelerometer off
//   DM_MOTION_AXES=x,-y,-z        the axis map from the controller's frame to the Switch's, see dm_pad_core.h
//   DM_MOTION_LOG=1               log a sample of the motion twice a second (to check the axes on a real device)
//   DM_RUMBLE=0                   no rumble
//   DM_RUMBLE_GAIN=1.0            rumble strength (0..2; the result is limited to full strength)
#pragma once
#include <stdint.h>

typedef struct { uint64_t buttons; int32_t lx, ly, rx, ry; } dm_ctrl_state;   // buttons: dm_input.h bits; sticks ±32767, up positive
typedef struct {
    void (*state)(const dm_ctrl_state *s);   // the physical controller's buttons and sticks changed (any thread)
    void (*presence)(int connected);         // at least one physical controller is connected / none is (any thread)
} dm_ctrl_ops;

// Start reading controllers (main thread, once). Chooses the backend and logs which
void dm_ctrl_start(const dm_ctrl_ops *ops);

// ---- for the backends ----
int dm_ctrl_face_layout(void);                                            // DM_FACE_XBOX / DM_FACE_NINTENDO
int dm_ctrl_motion_enabled(void);
int dm_ctrl_rumble_enabled(void);
float dm_ctrl_rumble_gain(void);
// One motion sample in frame D (x right, y toward the top edge, z out of the face; acceleration as the force the sensor feels, in G; angular velocity in rad/s): mapped to the Switch frame and given to the host
void dm_ctrl_motion(const float force_g[3], const float gyro_rps[3]);
void dm_ctrl_motion_off(void);

// SDL3 backend (DmSDL3Pad.c). dm_sdl3_available is 0 when the app was built without SDL3; dm_sdl3_start then does nothing and returns 0. A 1 means the backend runs (it owns the controllers)
int dm_sdl3_available(void);
int dm_sdl3_start(const dm_ctrl_ops *ops);
