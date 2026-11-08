/*
 * Touch input: host pointer events become calls of the profile's touch hook
 * (droidlift.h), which passes them to the game's own natives.
 *
 * As on Android, touches are queued as they arrive and delivered on the
 * render thread before the next frame (the game's frame hook calls
 * input_dispatch where its Java side would run GLSurfaceView.queueEvent
 * work). x and y are pixels of the game's rectangle of the surface, origin
 * top-left; time is seconds of uptime, like MotionEvent.getEventTime() /
 * 1000.
 *
 * Sources: the window's mouse (gfx.c: a held left button is touch 0), and a
 * script for the smoke tests (--touch), given in surface pixels too.
 *
 * With a pillarbox (input_set_content), touches arrive in pixels of the whole
 * surface and are moved into the game's rectangle.
 */
#ifndef INPUT_H
#define INPUT_H

#include "cpu.h"
#include "so_util.h"

typedef enum { TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP, TOUCH_CANCEL } touch_kind;

/* Pillarbox: the game sees only the w x h rectangle at (x, y) of the surface
 * (top-left origin). Later touches are made relative to it, and touches
 * outside it are moved to its nearest edge. */
void input_set_content(int x, int y, int w, int h);

/* Queues one touch event for the next frame, stamped with the current time. */
void input_touch(touch_kind kind, int id, float x, float y);

/* Delivers the queued touches to the game in arrival order, through the
 * profile's touch hook (call it from the frame hook). Returns 0, or -1
 * on a fault. */
int input_dispatch(cpu *c, const so_module *main);

/* Scripted touches (--touch), for the tests. A spec is a list of items
 * separated by ';':
 *
 *     WHEN:ACTION:X,Y
 *
 * ACTION is down, move, up or tap (down, then up two frames later); X,Y are
 * surface pixels. WHEN is a frame number (0-based, counted from the first
 * nativeRender), or a wait for the game:
 *
 *     SIGNAL[#K][+N]       N frames (default 0) after the frame in which the
 *                          game sent SIGNAL for the K-th time (default 1st);
 *                          +0 and +1 both mean the next frame
 *
 * Either may be followed by a repeat, words separated by spaces:
 *
 *     every N              again every N frames (N >= 3 for tap), or
 *     every Nms            every N ms of wall clock (the game's own time:
 *                          a rhythm that holds on a slow machine) ...
 *     until SIGNAL         ... until the game has sent SIGNAL (from this
 *                          item's start on; earlier ones don't count)
 *     times K              ... at most K times
 *
 * e.g. "game_load_complete+45:tap:1760,840" (ACCEPT once the Terms are up),
 * "game_load_complete+200 every 60 until game_start:tap:1500,300" (tap the
 * title until a run starts), "1000 every 40 times 36:tap:200,1000".
 *
 * Signals are the game's own (input_signal): its analytics events by name
 * (game_load_complete, game_start, game_end_paused...) and the loading wheel
 * (loading_wheel_on, loading_wheel_off). Frames are paced to 60 fps at most,
 * so N frames are at least N/60 s of the game's wall clock.
 *
 * Returns 0, or -1 on a malformed spec. May be called more than once. */
int input_script_add(const char *spec);

/* Records that the game sent a signal (any thread). Logged as
 * "[signal] frame F: NAME". */
void input_signal(const char *name);

/* Call once per frame boundary, in order, before frame `frame` renders
 * (after the previous frame's nativeRender, before its swap): fixes which
 * scripted touches frame `frame` gets, from the signals sent so far (later
 * ones count for the next frame), and returns how many. */
int input_script_plan(int frame);

/* Emits the scripted touches planned for frame, as (kind, x, y) via emit.
 * Window mode passes them through the window's event queue so they take the
 * mouse's path. */
void input_script_frame(int frame, void (*emit)(touch_kind kind, float x, float y));

/* 1 if every scripted item has run all its touches (a repeat with neither
 * until nor times never finishes). */
int input_script_done(void);

/* Frame of the last scripted touch so far, or -1 if none ran yet. */
int input_script_last_touch(void);

/* Logs the items still waiting (and on which signal) to stderr; returns
 * their number. */
int input_script_report_pending(void);

/* Checks the script parser and planner against made-up signals (no game):
 * the frame syntax, waits, repeats and malformed items. Returns 0 if all
 * pass. Leaves no script behind. */
int input_script_selftest(void);

/* Touch calls delivered to the game so far. */
int input_delivered(void);

#endif
