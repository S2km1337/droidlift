#ifndef JNI_SELFTEST_H
#define JNI_SELFTEST_H

#include "cpu.h"

/* Runs the fake-JNI checks through the guest JNIEnv; 0 if all pass. */
int jni_selftest(cpu *c);

#endif
