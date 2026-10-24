/**
 * sandbox.h - Sandbox mode: per-mission settings chosen on the difficulty
 * screen (empty level, objectives off, level exits off) and the in-game spawn
 * menu (B on the keyboard, RB on a controller). port/src/sandbox.c.
 *
 * No SDL here: input_pc.c translates keys and controller state into the
 * SANDBOX_KEY_* / SBPAD_* values below, as it does for the tilde console.
 */
#ifndef PORT_SANDBOX_H
#define PORT_SANDBOX_H

#ifndef TARGET_N64

#include <ultra64.h>

/* the difficulty-screen toggles (front.c draws and flips them) */
extern s32 g_SandboxEmptyLevel;
extern s32 g_SandboxNoObjectives;
extern s32 g_SandboxNoExits;

/* front.c: a mission was started from the difficulty screen, so the toggles
 * above apply to the next stage load */
void sandboxArmForNextMission(void);

/* lv.c lvlStageLoad: latch (or clear) this mission's settings, drop the
 * previous mission's spawn records */
void sandboxStageLoad(s32 stage);

/* the latched settings for the running mission (solo only) */
s32 sandboxEmptyActive(void);
s32 sandboxNoObjectivesActive(void);
s32 sandboxNoExitsActive(void);

/* prop.c proplvreset2: 1 = leave this setup entry out (empty level) */
s32 sandboxSkipPropdef(s32 type);

/* lv.c, once per game tick while a stage runs */
void sandboxTick(void);

/* boss.c, after the frame and before the console */
Gfx *sandboxRender(Gfx *gdl);

/* 1 while the spawn menu owns the input (osContGetReadData / videoGetPad
 * blank the game's view of every pad, the keyboard and the mouse) */
s32 portSandboxCapturing(void);

/* chrai.c: an AI list is about to run a level-exit command in a mission with
 * exits off. Returns 1 when the caller must stop that list instead. */
s32 sandboxBlockLevelExit(void);

/* Keys the menu consumes; input_pc.c maps SDL scancodes onto these. */
enum SandboxKey {
    SANDBOX_KEY_TOGGLE = 1, /* B */
    SANDBOX_KEY_UP,
    SANDBOX_KEY_DOWN,
    SANDBOX_KEY_LEFT,
    SANDBOX_KEY_RIGHT,
    SANDBOX_KEY_ACCEPT,     /* Enter, Space, E */
    SANDBOX_KEY_BACK,       /* Backspace, Escape */
    SANDBOX_KEY_PAGEUP,
    SANDBOX_KEY_PAGEDOWN
};

/* input_pc.c: a key went down. Returns 1 when the sandbox took it (the
 * caller must not pass it on). */
s32 sandboxKeyDown(s32 key);

/* Controller state, ORed over every open pad (input_pc.c portSandboxPadButtons) */
#define SBPAD_UP    0x0001
#define SBPAD_DOWN  0x0002
#define SBPAD_LEFT  0x0004
#define SBPAD_RIGHT 0x0008
#define SBPAD_A     0x0010
#define SBPAD_B     0x0020
#define SBPAD_RB    0x0040 /* opens / closes the menu */
#define SBPAD_LT    0x0080 /* page up in the menu */
#define SBPAD_RT    0x0100 /* page down in the menu */
u32 portSandboxPadButtons(void);

#endif /* TARGET_N64 */
#endif /* PORT_SANDBOX_H */
