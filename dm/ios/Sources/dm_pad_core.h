// Controller core: the parts of the physical-controller path that need no Apple or SDL headers, so they are unit-tested on any machine (dm/host/tests/t_pad_core.c).
// Used by DmController.m (GameController backend), DmSDL3Pad.c (SDL3 backend) and DmGamepad.m.
#pragma once
#include <stdint.h>

// ---- Face buttons ----
// The face buttons arrive by position (south = bottom, east = right, west = left, north = top), which is how both GameController and SDL3 name them.
//   DM_FACE_XBOX     (the default) Xbox layout: A bottom, B right, X left, Y top. The game's A is the bottom key, B the right one, X the left one and Y the top one.
//   DM_FACE_NINTENDO Nintendo positions, what the app did before: A right, B bottom, X top, Y left.
// The touch gamepad keeps the Nintendo positions in both cases (DmGamepad.m); this only decides what a physical controller's keys mean.
enum { DM_FACE_XBOX = 0, DM_FACE_NINTENDO = 1 };
// "xbox" / "nintendo" (also "switch", "position"), any case; anything else (and NULL) gives dflt
int dm_pad_face_parse(const char *s, int dflt);
// The game's A / B / X / Y bits (dm_input.h DM_BTN_A..Y: bits 0..3) for the four keys by position
uint64_t dm_pad_face_buttons(int layout, int south, int east, int west, int north);

// ---- Sticks ----
// -1..1 → -32767..32767 (the host's range). Clamps; NaN → 0
int32_t dm_pad_axis(float v);

// ---- Motion axes ----
// The host's six-axis state uses the Switch controller frame, in which a controller lying flat, face up, reads acceleration (0, 0, -1) G (hle_hid.c): x to the right, y toward the player, z down into the body.
// The front ends first bring a sample to the frame D that SDL3 uses (x right, y toward the top edge / away from the player, z out of the face), with acceleration as the force the sensor feels
// (flat face up: (0, 0, +1) G; GameController reports the gravity vector instead, which is the negative: DmController.m negates it). D → Switch frame is a half turn about x: (x, -y, -z), for acceleration and
// angular velocity alike. An axis map says it per output axis: out[i] = sgn[i] * in[src[i]]. DM_MOTION_AXES (Documents/env.txt) replaces the default if a controller's axes turn out different on a real device.
typedef struct { int8_t src[3]; int8_t sgn[3]; } dm_axmap;
extern const dm_axmap DM_AXMAP_DEFAULT;   // "x,-y,-z"
// "x,-y,-z": three tokens, each x, y or z with an optional + or - sign, every axis exactly once (spaces are ignored). Returns 1 and fills out, or 0 and leaves out alone
int dm_axmap_parse(const char *s, dm_axmap *out);
void dm_axmap_apply(const dm_axmap *m, const float in[3], float out[3]);

// ---- HD rumble ----
// A Switch vibration value holds two bands, each an amplitude 0..1 and a frequency in Hz (40..1252; the defaults are 160 Hz low and 320 Hz high).
// A Core Haptics event has an intensity and a sharpness, both 0..1: the band's amplitude and its frequency on a log scale over 40..1252 Hz.
void dm_hd_to_haptic(float amp, float freq_hz, float *intensity, float *sharpness);
// Amplitude 0..1 → 0..65535 (SDL_RumbleGamepad's scale)
uint16_t dm_hd_to_u16(float amp);
