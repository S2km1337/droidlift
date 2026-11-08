#ifndef LIFECYCLE_H
#define LIFECYCLE_H

#include "cpu.h"

/* From now on the host app's state changes (iOS: resign active, background,
 * foreground, terminate) drive the game's lifecycle on cpu c (dl_app_pause,
 * dl_app_stop, dl_app_resume). Called once the surface exists. */
void lifecycle_attach(cpu *c);

#endif
