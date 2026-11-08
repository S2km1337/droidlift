/*
 * The iOS app's log: stdout and stderr, which go nowhere on a
 * device, are copied to <container>/Documents/logs/run-<time>.log (the
 * last 5 runs are kept) and to os_log (subsystem: the bundle id, category
 * "loader"). Where stdout already reaches a console
 * (the Simulator, `devicectl device process launch --console`), it still
 * does.
 */
#ifndef IOS_LOG_H
#define IOS_LOG_H

/* Call first thing in main, before anything prints. */
void ios_log_start(void);

/* This run's log file, or NULL if it couldn't be made. */
const char *ios_log_path(void);

/* Waits (up to a second) until everything printed so far is in the file
 * and os_log. For before _exit, which skips atexit. */
void ios_log_flush(void);

#endif
