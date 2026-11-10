#ifndef KUSER_H
#define KUSER_H

/* Maps the kernel user helper page at 0xffff0000 (kuser.c), for armeabi
 * code. Call after gm_init. Returns 0, or -1 if it can't be placed. */
int kuser_init(void);

#endif
