#ifndef STARTUP_H
#define STARTUP_H

#include "cpu.h"
#include "so_util.h"

/* Runs a library's .init_array constructors, as Android's linker does after
 * loading it (DL_TRACE_INIT=1 logs each). Returns 0, or -1 on a fault. */
int so_run_init_array(cpu *c, const so_module *m);

/* Calls m's JNI_OnLoad(vm, NULL) if it exports one, as System.loadLibrary
 * does right after the constructors (natives it registers are then found by
 * dl_native). Returns 0, or -1 on a fault. */
int so_run_jni_onload(cpu *c, const so_module *m);

/* The app's place in Android's activity lifecycle and the steps between
 * states (droidlift.h: dl_app_pause, dl_app_stop, dl_app_resume). */

#endif
