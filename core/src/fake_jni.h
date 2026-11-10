/*
 * Fake Java VM for the game's JNI calls, in the style of FalsoJNI.
 *
 * The JavaVM and JNIEnv function tables live in guest memory. Each slot points
 * to a guest thunk
 *     svc #(JNI_SVC_BASE | slot)       (JNIEnv, slot 0..232)
 *     svc #(JNI_SVC_VM | slot)         (JavaVM invoke interface, slot 0..7)
 *     bx  lr
 * which cpu_svc routes to jni_dispatch. Java objects are host-side records
 * behind opaque 32-bit handles; primitive array data lives in guest memory so
 * Get<Type>ArrayElements can hand out direct pointers.
 *
 * Classes the game's Java shell really has are found by FindClass; any other
 * class returns NULL with a pending NoClassDefFoundError, as on Android when a
 * class isn't in classes.dex; native code usually tolerates that. Every class, method and field lookup is logged with its signature.
 * Java methods are implemented by host functions registered by name; a call to
 * a method nobody implemented logs once, returns 0 and counts as an
 * unimplemented call, like a dynlib logging stub.
 */
#ifndef FAKE_JNI_H
#define FAKE_JNI_H

#include <stdint.h>

struct cpu;

#define JNI_SVC_BASE 0x400000u
#define JNI_SVC_VM 0x410000u
#define JNI_SVC_MASK 0xFF0000u

#define JNI_VERSION_1_6 0x00010006

/* A Java method call being serviced by a host implementation. Arguments are
 * decoded from the method signature: integral types and references widened to
 * 64 bits, float and double as their IEEE bit patterns (f32 in the low word). */
typedef struct jni_call {
  uint32_t self;  /* jobject for instance methods and constructors, jclass for static */
  int nargs;
  uint64_t args[24]; /* cocos2d-x's createTextBitmapShadowStroke takes 19 */
  /* The cpu that made the call, for implementations that call back into the
   * game (cpu_callback), as Java code calls a native method. */
  struct cpu *cpu;
} jni_call;

/* Returns the method's result (same encoding as the arguments; ignored for void). */
typedef uint64_t (*jni_method_fn)(const jni_call *call);

/* Builds the guest tables. Call after gm_init. */
int jni_init(void);
/* The game's classes FindClass resolves, besides the built-in android/java
 * basics: a NULL-terminated list that must outlive the VM. */
void jni_add_known_classes(const char *const *names);

/* Guest addresses of the JNIEnv* and JavaVM* handed to Java_* entry points. */
uint32_t jni_env(void);
uint32_t jni_vm(void);

/* Registers the host implementation of class.name with the given JNI signature,
 * e.g. ("com/example/Natives", "isTablet", "()Z", fn). The class must be one
 * FindClass knows; returns -1 otherwise. Registering "<init>" implements a
 * constructor (its result is ignored). */
int jni_register_method(const char *cls, const char *name, const char *sig, jni_method_fn fn);

/* Makes lookups of class.name with signature sig fail with NULL and a pending
 * NoSuchMethodError, for methods native code probes for that the real class
 * doesn't have. Returns -1 if the class is unknown. */
int jni_mark_method_absent(const char *cls, const char *name, const char *sig);

/* Host-side object helpers for method implementations and the host shell. */
uint32_t jni_new_string(const char *utf8);
const char *jni_string_utf8(uint32_t str); /* NULL unless str is a String */
uint32_t jni_new_instance(const char *cls); /* 0 if the class is unknown */
uint32_t jni_find_class(const char *cls);   /* the jclass, as FindClass returns it */
uint32_t jni_new_object_array(const char *elem_cls, uint32_t len);
int jni_set_object_array_element(uint32_t array, uint32_t index, uint32_t value);
/* A new zeroed primitive array of elem ('Z', 'B', 'C', 'S', 'I', 'J', 'F' or
 * 'D') and len elements, as New<Type>Array makes. */
uint32_t jni_new_prim_array(char elem, uint32_t len);
/* Frees a primitive array's data once no native code holds it (for host code
 * that makes big temporary arrays, such as text bitmaps); the handle stays,
 * as an empty array. */
void jni_free_prim_array(uint32_t array);
/* Primitive arrays: guest address and length of the data, or 0 if not one. */
uint32_t jni_array_data(uint32_t array, uint32_t *len);

/* Pending exception (0 if none), as ExceptionOccurred sees it. */
uint32_t jni_pending_exception(void);

/* Called through cpu_svc for svc #(JNI_SVC_BASE | slot) and #(JNI_SVC_VM | slot). */
void jni_dispatch(struct cpu *c, uint32_t imm);
uint32_t jni_unimplemented_calls(void);

/* A native method a library registered with RegisterNatives (usually from
 * JNI_OnLoad), by class, name and signature (NULL: any); 0 if none. */
uint32_t jni_native(const char *cls, const char *name, const char *sig);

#endif
