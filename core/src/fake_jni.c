/*
 * Fake Java VM: see fake_jni.h.
 */
#include "fake_jni.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "guest_mem.h"
#include "reimpl/reimpl.h"

#define N_ENV_SLOTS 233 /* JNI 1.6, as on Android (no GetModule) */
#define N_VM_SLOTS 8

/* JNINativeInterface slot names, in table order (jni.h). */
static const char *const env_slot_names[N_ENV_SLOTS] = {
    NULL, NULL, NULL, NULL, "GetVersion",
    "DefineClass", "FindClass", "FromReflectedMethod", "FromReflectedField", "ToReflectedMethod",
    "GetSuperclass", "IsAssignableFrom", "ToReflectedField", "Throw", "ThrowNew",
    "ExceptionOccurred", "ExceptionDescribe", "ExceptionClear", "FatalError", "PushLocalFrame",
    "PopLocalFrame", "NewGlobalRef", "DeleteGlobalRef", "DeleteLocalRef", "IsSameObject",
    "NewLocalRef", "EnsureLocalCapacity", "AllocObject", "NewObject", "NewObjectV",
    "NewObjectA", "GetObjectClass", "IsInstanceOf", "GetMethodID", "CallObjectMethod",
    "CallObjectMethodV", "CallObjectMethodA", "CallBooleanMethod", "CallBooleanMethodV", "CallBooleanMethodA",
    "CallByteMethod", "CallByteMethodV", "CallByteMethodA", "CallCharMethod", "CallCharMethodV",
    "CallCharMethodA", "CallShortMethod", "CallShortMethodV", "CallShortMethodA", "CallIntMethod",
    "CallIntMethodV", "CallIntMethodA", "CallLongMethod", "CallLongMethodV", "CallLongMethodA",
    "CallFloatMethod", "CallFloatMethodV", "CallFloatMethodA", "CallDoubleMethod", "CallDoubleMethodV",
    "CallDoubleMethodA", "CallVoidMethod", "CallVoidMethodV", "CallVoidMethodA", "CallNonvirtualObjectMethod",
    "CallNonvirtualObjectMethodV", "CallNonvirtualObjectMethodA", "CallNonvirtualBooleanMethod", "CallNonvirtualBooleanMethodV", "CallNonvirtualBooleanMethodA",
    "CallNonvirtualByteMethod", "CallNonvirtualByteMethodV", "CallNonvirtualByteMethodA", "CallNonvirtualCharMethod", "CallNonvirtualCharMethodV",
    "CallNonvirtualCharMethodA", "CallNonvirtualShortMethod", "CallNonvirtualShortMethodV", "CallNonvirtualShortMethodA", "CallNonvirtualIntMethod",
    "CallNonvirtualIntMethodV", "CallNonvirtualIntMethodA", "CallNonvirtualLongMethod", "CallNonvirtualLongMethodV", "CallNonvirtualLongMethodA",
    "CallNonvirtualFloatMethod", "CallNonvirtualFloatMethodV", "CallNonvirtualFloatMethodA", "CallNonvirtualDoubleMethod", "CallNonvirtualDoubleMethodV",
    "CallNonvirtualDoubleMethodA", "CallNonvirtualVoidMethod", "CallNonvirtualVoidMethodV", "CallNonvirtualVoidMethodA", "GetFieldID",
    "GetObjectField", "GetBooleanField", "GetByteField", "GetCharField", "GetShortField",
    "GetIntField", "GetLongField", "GetFloatField", "GetDoubleField", "SetObjectField",
    "SetBooleanField", "SetByteField", "SetCharField", "SetShortField", "SetIntField",
    "SetLongField", "SetFloatField", "SetDoubleField", "GetStaticMethodID", "CallStaticObjectMethod",
    "CallStaticObjectMethodV", "CallStaticObjectMethodA", "CallStaticBooleanMethod", "CallStaticBooleanMethodV", "CallStaticBooleanMethodA",
    "CallStaticByteMethod", "CallStaticByteMethodV", "CallStaticByteMethodA", "CallStaticCharMethod", "CallStaticCharMethodV",
    "CallStaticCharMethodA", "CallStaticShortMethod", "CallStaticShortMethodV", "CallStaticShortMethodA", "CallStaticIntMethod",
    "CallStaticIntMethodV", "CallStaticIntMethodA", "CallStaticLongMethod", "CallStaticLongMethodV", "CallStaticLongMethodA",
    "CallStaticFloatMethod", "CallStaticFloatMethodV", "CallStaticFloatMethodA", "CallStaticDoubleMethod", "CallStaticDoubleMethodV",
    "CallStaticDoubleMethodA", "CallStaticVoidMethod", "CallStaticVoidMethodV", "CallStaticVoidMethodA", "GetStaticFieldID",
    "GetStaticObjectField", "GetStaticBooleanField", "GetStaticByteField", "GetStaticCharField", "GetStaticShortField",
    "GetStaticIntField", "GetStaticLongField", "GetStaticFloatField", "GetStaticDoubleField", "SetStaticObjectField",
    "SetStaticBooleanField", "SetStaticByteField", "SetStaticCharField", "SetStaticShortField", "SetStaticIntField",
    "SetStaticLongField", "SetStaticFloatField", "SetStaticDoubleField", "NewString", "GetStringLength",
    "GetStringChars", "ReleaseStringChars", "NewStringUTF", "GetStringUTFLength", "GetStringUTFChars",
    "ReleaseStringUTFChars", "GetArrayLength", "NewObjectArray", "GetObjectArrayElement", "SetObjectArrayElement",
    "NewBooleanArray", "NewByteArray", "NewCharArray", "NewShortArray", "NewIntArray",
    "NewLongArray", "NewFloatArray", "NewDoubleArray", "GetBooleanArrayElements", "GetByteArrayElements",
    "GetCharArrayElements", "GetShortArrayElements", "GetIntArrayElements", "GetLongArrayElements", "GetFloatArrayElements",
    "GetDoubleArrayElements", "ReleaseBooleanArrayElements", "ReleaseByteArrayElements", "ReleaseCharArrayElements", "ReleaseShortArrayElements",
    "ReleaseIntArrayElements", "ReleaseLongArrayElements", "ReleaseFloatArrayElements", "ReleaseDoubleArrayElements", "GetBooleanArrayRegion",
    "GetByteArrayRegion", "GetCharArrayRegion", "GetShortArrayRegion", "GetIntArrayRegion", "GetLongArrayRegion",
    "GetFloatArrayRegion", "GetDoubleArrayRegion", "SetBooleanArrayRegion", "SetByteArrayRegion", "SetCharArrayRegion",
    "SetShortArrayRegion", "SetIntArrayRegion", "SetLongArrayRegion", "SetFloatArrayRegion", "SetDoubleArrayRegion",
    "RegisterNatives", "UnregisterNatives", "MonitorEnter", "MonitorExit", "GetJavaVM",
    "GetStringRegion", "GetStringUTFRegion", "GetPrimitiveArrayCritical", "ReleasePrimitiveArrayCritical", "GetStringCritical",
    "ReleaseStringCritical", "NewWeakGlobalRef", "DeleteWeakGlobalRef", "ExceptionCheck", "NewDirectByteBuffer",
    "GetDirectBufferAddress", "GetDirectBufferCapacity", "GetObjectRefType",};

static const char *const vm_slot_names[N_VM_SLOTS] = {
    NULL, NULL, NULL, "DestroyJavaVM", "AttachCurrentThread", "DetachCurrentThread", "GetEnv",
    "AttachCurrentThreadAsDaemon",
};

/* Classes FindClass resolves: the android/java/org.json basics native game
 * code commonly looks up, plus the game's own (jni_add_known_classes: the
 * FindClass strings in its libraries that exist in its classes.dex). Any
 * other class is missing, as it would be on Android. */
static const char *const known_classes[] = {
    "android/content/res/AssetManager",
    "android/graphics/Bitmap",
    "android/graphics/BitmapFactory",
    "android/os/Bundle",
    "java/lang/Class",
    "java/lang/Object",
    "java/lang/String",
    "java/lang/Throwable",
    "java/nio/Buffer",
    "java/security/KeyFactory",
    "java/security/Signature",
    "java/security/spec/X509EncodedKeySpec",
    "java/util/ArrayList",
    "org/json/JSONArray",
    "org/json/JSONObject",
};

static const char *const *extra_classes;

void jni_add_known_classes(const char *const *names) { extra_classes = names; }


/* Handles are opaque to native code; distinct bases catch mix-ups in logs. */
#define OBJ_BASE 0x00010000u
#define MID_BASE 0x20000000u
#define FID_BASE 0x30000000u

typedef enum { OBJ_CLASS = 1, OBJ_INSTANCE, OBJ_STRING, OBJ_ARRAY } obj_kind;

typedef struct jobj {
  obj_kind kind;
  int cls;    /* index into classes: the object's class (OBJ_CLASS: the class it names) */
  char *utf8; /* OBJ_STRING */
  char elem;  /* OBJ_ARRAY: element signature char, 'L' for references */
  uint32_t len;
  uint32_t data;  /* primitive OBJ_ARRAY: guest address of the elements */
  uint32_t *refs; /* reference OBJ_ARRAY */
} jobj;

typedef struct jcls {
  char *name;
  uint32_t handle; /* its java/lang/Class object */
} jcls;

typedef struct jmeth {
  int cls;
  char *name, *sig;
  jni_method_fn fn;
  int warned;
  int absent; /* not in the real class: lookups fail like on Android */
} jmeth;

typedef struct jfield {
  int cls;
  char *name, *sig;
} jfield;

static jobj *objs;
static jcls *classes;
static jmeth *methods;
static jfield *fields;
static uint32_t n_objs, n_classes, n_methods, n_fields;
static uint32_t cap_objs, cap_classes, cap_methods, cap_fields;

static uint32_t env_addr, vm_addr, pending;
static uint32_t unimplemented_calls;

static void *grow(void *p, uint32_t n, uint32_t *cap, size_t elem) {
  if (n < *cap) return p;
  *cap = *cap ? *cap * 2 : 64;
  p = realloc(p, *cap * elem);
  if (!p) {
    fprintf(stderr, "[jni] out of host memory\n");
    abort();
  }
  return p;
}

static jobj *obj_get(uint32_t h) {
  if (h < OBJ_BASE || h - OBJ_BASE >= n_objs) return NULL;
  return &objs[h - OBJ_BASE];
}

static uint32_t obj_new(obj_kind kind, int cls) {
  objs = grow(objs, n_objs, &cap_objs, sizeof *objs);
  objs[n_objs] = (jobj){.kind = kind, .cls = cls};
  return OBJ_BASE + n_objs++;
}

static int is_known_class(const char *name) {
  for (size_t i = 0; i < sizeof(known_classes) / sizeof(known_classes[0]); i++)
    if (strcmp(known_classes[i], name) == 0) return 1;
  for (const char *const *e = extra_classes; e && *e; e++)
    if (strcmp(*e, name) == 0) return 1;
  return 0;
}

/* Index of the class, created on first use. Array classes ("[B", "[Ljava/...;")
 * and the exceptions the fake VM throws are created internally as needed. */
static int class_index(const char *name) {
  for (uint32_t i = 0; i < n_classes; i++)
    if (strcmp(classes[i].name, name) == 0) return (int)i;
  classes = grow(classes, n_classes, &cap_classes, sizeof *classes);
  int idx = (int)n_classes++;
  classes[idx].name = strdup(name);
  classes[idx].handle = obj_new(OBJ_CLASS, idx);
  return idx;
}

static int class_of_handle(uint32_t h) {
  jobj *o = obj_get(h);
  return o && o->kind == OBJ_CLASS ? o->cls : -1;
}

static void throw_new(const char *cls, const char *msg) {
  fprintf(stderr, "[jni] throwing %s: %s\n", cls, msg);
  pending = obj_new(OBJ_INSTANCE, class_index(cls));
}

/* ---- strings: stored as UTF-8 (Java's modified UTF-8 for the game's data) */

static uint32_t utf8_to_utf16(const char *s, uint16_t *out) {
  uint32_t n = 0;
  const uint8_t *p = (const uint8_t *)s;
  while (*p) {
    uint32_t cp;
    if (*p < 0x80) cp = *p++;
    else if ((*p & 0xE0) == 0xC0 && p[1]) cp = (uint32_t)(*p & 0x1F) << 6 | (p[1] & 0x3F), p += 2;
    else if ((*p & 0xF0) == 0xE0 && p[1] && p[2])
      cp = (uint32_t)(*p & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F), p += 3;
    else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3])
      cp = (uint32_t)(*p & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 |
           (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F),
      p += 4;
    else cp = 0xFFFD, p++;
    if (cp >= 0x10000) {
      if (out) out[n] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10)),
               out[n + 1] = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
      n += 2;
    } else {
      if (out) out[n] = (uint16_t)cp;
      n++;
    }
  }
  return n;
}

/* Modified UTF-8, one unit at a time (surrogates stay 3-byte sequences, as in
 * Java's own encoding). out needs 3 * len + 1 bytes. */
static uint32_t utf16_to_utf8(const uint16_t *s, uint32_t len, char *out) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < len; i++) {
    uint16_t u = s[i];
    if (u && u < 0x80) {
      out[n++] = (char)u;
    } else if (u < 0x800) {
      out[n++] = (char)(0xC0 | u >> 6);
      out[n++] = (char)(0x80 | (u & 0x3F));
    } else {
      out[n++] = (char)(0xE0 | u >> 12);
      out[n++] = (char)(0x80 | ((u >> 6) & 0x3F));
      out[n++] = (char)(0x80 | (u & 0x3F));
    }
  }
  out[n] = 0;
  return n;
}

static jobj *string_get(uint32_t h) {
  jobj *o = obj_get(h);
  return o && o->kind == OBJ_STRING ? o : NULL;
}

uint32_t jni_new_string(const char *utf8) {
  uint32_t h = obj_new(OBJ_STRING, class_index("java/lang/String"));
  objs[h - OBJ_BASE].utf8 = strdup(utf8);
  return h;
}

const char *jni_string_utf8(uint32_t str) {
  jobj *o = string_get(str);
  return o ? o->utf8 : NULL;
}

/* UTF-16 copy of a string in host memory (caller frees); *len gets its length. */
static uint16_t *string_utf16(jobj *o, uint32_t *len) {
  *len = utf8_to_utf16(o->utf8, NULL);
  uint16_t *u = malloc((*len + 1) * sizeof *u);
  utf8_to_utf16(o->utf8, u);
  return u;
}

/* ---- arrays */

static uint32_t elem_size(char t) {
  switch (t) {
  case 'Z': case 'B': return 1;
  case 'C': case 'S': return 2;
  case 'J': case 'D': return 8;
  default: return 4; /* I F L */
  }
}

static uint32_t array_new(char elem, const char *elem_cls, uint32_t len) {
  char name[256];
  if (elem == 'L') {
    if (elem_cls[0] == '[') snprintf(name, sizeof name, "[%s", elem_cls);
    else snprintf(name, sizeof name, "[L%s;", elem_cls);
  } else {
    snprintf(name, sizeof name, "[%c", elem);
  }
  uint32_t h = obj_new(OBJ_ARRAY, class_index(name));
  jobj *o = &objs[h - OBJ_BASE];
  o->elem = elem;
  o->len = len;
  if (elem == 'L') {
    o->refs = calloc(len ? len : 1, sizeof *o->refs);
  } else {
    uint32_t bytes = len * elem_size(elem);
    o->data = guest_malloc(bytes ? bytes : 1);
    memset(g2h(o->data), 0, bytes);
  }
  return h;
}

static jobj *array_get(uint32_t h) {
  jobj *o = obj_get(h);
  return o && o->kind == OBJ_ARRAY ? o : NULL;
}

uint32_t jni_new_object_array(const char *elem_cls, uint32_t len) {
  return array_new('L', elem_cls, len);
}

int jni_set_object_array_element(uint32_t array, uint32_t index, uint32_t value) {
  jobj *a = array_get(array);
  if (!a || a->elem != 'L' || index >= a->len) return -1;
  a->refs[index] = value;
  return 0;
}

uint32_t jni_array_data(uint32_t array, uint32_t *len) {
  jobj *a = array_get(array);
  if (!a || a->elem == 'L') return 0;
  if (len) *len = a->len;
  return a->data;
}

uint32_t jni_new_instance(const char *cls) {
  if (!is_known_class(cls)) return 0;
  return obj_new(OBJ_INSTANCE, class_index(cls));
}

uint32_t jni_find_class(const char *cls) {
  if (!is_known_class(cls)) return 0;
  return classes[class_index(cls)].handle;
}

uint32_t jni_pending_exception(void) { return pending; }

/* ---- methods and fields */

static uint32_t method_find(int cls, const char *name, const char *sig) {
  for (uint32_t i = 0; i < n_methods; i++)
    if (methods[i].cls == cls && strcmp(methods[i].name, name) == 0 &&
        strcmp(methods[i].sig, sig) == 0)
      return i;
  methods = grow(methods, n_methods, &cap_methods, sizeof *methods);
  methods[n_methods] = (jmeth){.cls = cls, .name = strdup(name), .sig = strdup(sig)};
  return n_methods++;
}

int jni_register_method(const char *cls, const char *name, const char *sig, jni_method_fn fn) {
  if (!is_known_class(cls)) {
    fprintf(stderr, "[jni] cannot implement %s.%s: unknown class\n", cls, name);
    return -1;
  }
  uint32_t i = method_find(class_index(cls), name, sig); /* may move methods */
  methods[i].fn = fn;
  return 0;
}

int jni_mark_method_absent(const char *cls, const char *name, const char *sig) {
  if (!is_known_class(cls)) return -1;
  uint32_t i = method_find(class_index(cls), name, sig);
  methods[i].absent = 1;
  return 0;
}

static jmeth *method_get(uint32_t h) {
  if (h < MID_BASE || h - MID_BASE >= n_methods) return NULL;
  return &methods[h - MID_BASE];
}

/* GetMethodID / GetStaticMethodID: any method of a known class resolves (the
 * dex is the authority on what exists, and the game only asks for methods its
 * Java shell has); a bad class handle yields NULL and NoSuchMethodError. */
static uint32_t get_method_id(uint32_t clsh, uint32_t name, uint32_t sig, int is_static) {
  int cls = class_of_handle(clsh);
  const char *n = g2h(name), *s = g2h(sig);
  if (cls < 0) {
    fprintf(stderr, "[jni] Get%sMethodID(<bad class 0x%x>, %s, %s) -> NULL\n",
            is_static ? "Static" : "", clsh, n, s);
    throw_new("java/lang/NoSuchMethodError", n);
    return 0;
  }
  uint32_t mid = MID_BASE + method_find(cls, n, s);
  if (method_get(mid)->absent) {
    fprintf(stderr, "[jni] Get%sMethodID(%s, %s, %s) -> NULL (no such method)\n",
            is_static ? "Static" : "", classes[cls].name, n, s);
    throw_new("java/lang/NoSuchMethodError", n);
    return 0;
  }
  fprintf(stderr, "[jni] Get%sMethodID(%s, %s, %s) -> 0x%x%s\n", is_static ? "Static" : "",
          classes[cls].name, n, s, mid, method_get(mid)->fn ? "" : " (no host implementation)");
  return mid;
}

static uint32_t get_field_id(uint32_t clsh, uint32_t name, uint32_t sig, int is_static) {
  int cls = class_of_handle(clsh);
  const char *n = g2h(name), *s = g2h(sig);
  if (cls < 0) {
    fprintf(stderr, "[jni] Get%sFieldID(<bad class 0x%x>, %s, %s) -> NULL\n",
            is_static ? "Static" : "", clsh, n, s);
    throw_new("java/lang/NoSuchFieldError", n);
    return 0;
  }
  for (uint32_t i = 0; i < n_fields; i++)
    if (fields[i].cls == cls && !strcmp(fields[i].name, n) && !strcmp(fields[i].sig, s))
      return FID_BASE + i;
  fields = grow(fields, n_fields, &cap_fields, sizeof *fields);
  fields[n_fields] = (jfield){.cls = cls, .name = strdup(n), .sig = strdup(s)};
  fprintf(stderr, "[jni] Get%sFieldID(%s, %s, %s) -> 0x%x\n", is_static ? "Static" : "",
          classes[cls].name, n, s, FID_BASE + n_fields);
  return FID_BASE + n_fields++;
}

/* ---- argument decoding for Call<Type>Method{,V,A} and NewObject{,V,A} */

typedef enum { ARGS_VARARGS, ARGS_VA_LIST, ARGS_JVALUES } args_mode;

typedef struct arg_reader {
  cpu *c;
  args_mode mode;
  uint32_t pos; /* VARARGS: AAPCS argument word index; otherwise a guest address */
} arg_reader;

static uint32_t next32(arg_reader *r) {
  if (r->mode == ARGS_VARARGS) return cpu_arg(r->c, (int)r->pos++);
  uint32_t v = gm_r32(r->pos);
  r->pos += 4;
  return v;
}

/* 64-bit values sit in an even register pair or on an 8-byte-aligned stack
 * slot; argument word 4 is the first stack word, so word parity matches. */
static uint64_t next64(arg_reader *r) {
  if (r->mode == ARGS_VARARGS) r->pos = (r->pos + 1) & ~1u;
  else r->pos = (r->pos + 7) & ~7u;
  uint64_t lo = next32(r);
  return lo | (uint64_t)next32(r) << 32;
}

static uint64_t read_arg(arg_reader *r, char t) {
  if (r->mode == ARGS_JVALUES) {
    uint32_t a = r->pos;
    r->pos += 8; /* sizeof(jvalue) */
    switch (t) {
    case 'Z': return *(uint8_t *)g2h(a);
    case 'B': return (uint64_t)(int64_t) * (int8_t *)g2h(a);
    case 'C': return gm_r32(a) & 0xFFFF;
    case 'S': return (uint64_t)(int64_t)(int16_t)gm_r32(a);
    case 'J': case 'D': return gm_r32(a) | (uint64_t)gm_r32(a + 4) << 32;
    default: return gm_r32(a); /* I F L */
    }
  }
  /* Variadic: char/short/boolean are promoted to int, float to double. */
  switch (t) {
  case 'J': case 'D': return next64(r);
  case 'F': {
    uint64_t bits = next64(r);
    double d;
    float f;
    memcpy(&d, &bits, 8);
    f = (float)d;
    uint32_t fb;
    memcpy(&fb, &f, 4);
    return fb;
  }
  case 'I': case 'L': return next32(r);
  case 'S': return (uint64_t)(int64_t)(int16_t)next32(r);
  case 'B': return (uint64_t)(int64_t)(int8_t)next32(r);
  case 'C': return next32(r) & 0xFFFF;
  default: return next32(r) & 0xFF; /* Z */
  }
}

/* Decodes the parameters of sig into call->args; returns the return type char. */
static char decode_args(const char *sig, arg_reader *r, jni_call *call) {
  const char *p = sig;
  if (*p == '(') p++;
  call->nargs = 0;
  while (*p && *p != ')') {
    char t = *p;
    if (t == '[') {
      while (*p == '[') p++;
      if (*p == 'L') p = strchr(p, ';');
      t = 'L';
    } else if (t == 'L') {
      p = strchr(p, ';');
    }
    if (!p) break;
    p++;
    uint64_t v = read_arg(r, t);
    if (call->nargs < (int)(sizeof call->args / sizeof call->args[0])) call->args[call->nargs++] = v;
  }
  return *p == ')' ? p[1] : 'V';
}

/* Runs method mid with arguments starting at AAPCS word `first` (VARARGS) or
 * at the va_list / jvalue array passed in that word. */
static uint64_t call_method(cpu *c, uint32_t self, uint32_t mid, int first, args_mode mode,
                            const char *what) {
  jmeth *m = method_get(mid);
  if (!m) {
    fprintf(stderr, "[jni] %s with bad method id 0x%x\n", what, mid);
    unimplemented_calls++;
    return 0;
  }
  jni_call call = {.self = self};
  arg_reader r = {c, mode, mode == ARGS_VARARGS ? (uint32_t)first : cpu_arg(c, first)};
  decode_args(m->sig, &r, &call);
  if (m->fn) return m->fn(&call);
  if (!m->warned) {
    fprintf(stderr, "[jni] %s %s.%s%s: no host implementation\n", what, classes[m->cls].name,
            m->name, m->sig);
    m->warned = 1;
  }
  if (strcmp(m->name, "<init>") == 0) return 0; /* a bare object is a fine default */
  unimplemented_calls++;
  return 0;
}

/* Call<Type>Method rows, in table order. */
static const char call_types[] = "LZBCSIJFDV";

static void set_result(cpu *c, char t, uint64_t v) {
  if (t == 'J' || t == 'D') cpu_ret64(c, v);
  else cpu_ret(c, t == 'V' ? 0 : (uint32_t)v);
}

/* Slots 34-93 and 114-143: Call[Nonvirtual|Static]<Type>Method{,V,A}. */
static int dispatch_call(cpu *c, uint32_t slot) {
  int nonvirtual = slot >= 64 && slot <= 93, is_static = slot >= 114 && slot <= 143;
  if (!(slot >= 34 && slot <= 63) && !nonvirtual && !is_static) return 0;
  uint32_t base = is_static ? 114 : nonvirtual ? 64 : 34;
  char t = call_types[(slot - base) / 3];
  args_mode mode = (args_mode)((slot - base) % 3);
  /* (env, obj|cls, mid, ...) or, nonvirtual, (env, obj, cls, mid, ...) */
  int mid_arg = nonvirtual ? 3 : 2;
  uint64_t v = call_method(c, cpu_arg(c, 1), cpu_arg(c, mid_arg), mid_arg + 1, mode,
                           env_slot_names[slot]);
  set_result(c, t, v);
  return 1;
}

/* Slots 175-214: New<Type>Array, Get/Release<Type>ArrayElements, Get/Set<Type>ArrayRegion. */
static int dispatch_prim_array(cpu *c, uint32_t slot) {
  static const char prim[] = "ZBCSIJFD";
  if (slot < 175 || slot > 214) return 0;
  uint32_t group = (slot - 175) / 8;
  char t = prim[(slot - 175) % 8];
  if (group == 0) { /* New<Type>Array(env, len) */
    cpu_ret(c, array_new(t, NULL, cpu_arg(c, 1)));
    return 1;
  }
  jobj *a = array_get(cpu_arg(c, 1));
  if (!a || a->elem != t) {
    fprintf(stderr, "[jni] %s on 0x%x, which is not a %c array\n", env_slot_names[slot],
            cpu_arg(c, 1), t);
    unimplemented_calls++;
    cpu_ret(c, 0);
    return 1;
  }
  if (group == 1) { /* Get<Type>ArrayElements(env, array, isCopy) */
    if (cpu_arg(c, 2)) *(uint8_t *)g2h(cpu_arg(c, 2)) = 0;
    cpu_ret(c, a->data);
  } else if (group == 2) { /* Release<Type>ArrayElements: the elements are the array */
  } else { /* Get/Set<Type>ArrayRegion(env, array, start, len, buf) */
    uint32_t start = cpu_arg(c, 2), len = cpu_arg(c, 3), buf = cpu_arg(c, 4);
    uint32_t es = elem_size(t);
    if ((uint64_t)start + len > a->len) {
      throw_new("java/lang/ArrayIndexOutOfBoundsException", env_slot_names[slot]);
    } else if (group == 3) {
      memmove(g2h(buf), g2h(a->data + start * es), (size_t)len * es);
    } else {
      memmove(g2h(a->data + start * es), g2h(buf), (size_t)len * es);
    }
  }
  return 1;
}

static void find_class(cpu *c) {
  const char *name = g2h(cpu_arg(c, 1));
  if (!is_known_class(name)) {
    fprintf(stderr, "[jni] FindClass(%s) -> NULL (not in the game's Java shell)\n", name);
    throw_new("java/lang/NoClassDefFoundError", name);
    cpu_ret(c, 0);
    return;
  }
  int cls = class_index(name); /* may move classes */
  uint32_t h = classes[cls].handle;
  fprintf(stderr, "[jni] FindClass(%s) -> 0x%x\n", name, h);
  cpu_ret(c, h);
}

static void new_object(cpu *c, uint32_t slot) {
  int cls = class_of_handle(cpu_arg(c, 1));
  if (cls < 0) {
    fprintf(stderr, "[jni] %s with bad class 0x%x\n", env_slot_names[slot], cpu_arg(c, 1));
    unimplemented_calls++;
    cpu_ret(c, 0);
    return;
  }
  uint32_t obj = obj_new(OBJ_INSTANCE, cls);
  if (slot != 27) /* AllocObject runs no constructor */
    call_method(c, obj, cpu_arg(c, 2), 3, (args_mode)(slot - 28), env_slot_names[slot]);
  cpu_ret(c, obj);
}

/* Natives a library registered (JNI_OnLoad's RegisterNatives), for
 * jni_native: the profile calls them like Java would. */
typedef struct jnative {
  int cls;
  char *name, *sig;
  uint32_t fn;
} jnative;
static jnative *natives;
static uint32_t n_natives, cap_natives;

static void register_natives(cpu *c) {
  int cls = class_of_handle(cpu_arg(c, 1));
  uint32_t tab = cpu_arg(c, 2), n = cpu_arg(c, 3);
  for (uint32_t i = 0; i < n; i++) { /* JNINativeMethod {name, signature, fnPtr} */
    const char *name = g2h(gm_r32(tab + i * 12)), *sig = g2h(gm_r32(tab + i * 12 + 4));
    uint32_t fn = gm_r32(tab + i * 12 + 8);
    fprintf(stderr, "[jni] RegisterNatives(%s): %s%s -> 0x%08x\n",
            cls >= 0 ? classes[cls].name : "<bad class>", name, sig, fn);
    if (cls < 0) continue;
    natives = grow(natives, n_natives, &cap_natives, sizeof *natives);
    natives[n_natives++] = (jnative){cls, strdup(name), strdup(sig), fn};
  }
  cpu_ret(c, cls < 0 ? (uint32_t)-1 : 0);
}

uint32_t jni_native(const char *cls, const char *name, const char *sig) {
  for (uint32_t i = n_natives; i-- > 0;) /* the latest registration wins */
    if (!strcmp(classes[natives[i].cls].name, cls) && !strcmp(natives[i].name, name) &&
        (!sig || !strcmp(natives[i].sig, sig)))
      return natives[i].fn;
  return 0;
}

static uint32_t guest_strdup(const char *s) {
  size_t n = strlen(s) + 1;
  uint32_t p = guest_malloc((uint32_t)n);
  if (p) memcpy(g2h(p), s, n);
  return p;
}

/* GetStringChars / GetStringCritical: a NUL-terminated UTF-16 copy. */
static uint32_t string_chars_copy(cpu *c, jobj *s) {
  uint32_t len;
  uint16_t *u = string_utf16(s, &len);
  uint32_t p = guest_malloc((len + 1) * 2);
  if (p) memcpy(g2h(p), u, (len + 1) * 2);
  free(u);
  if (cpu_arg(c, 2)) *(uint8_t *)g2h(cpu_arg(c, 2)) = 1;
  return p;
}

static void string_region(cpu *c, jobj *s, int utf8) {
  uint32_t start = cpu_arg(c, 2), n = cpu_arg(c, 3), buf = cpu_arg(c, 4), len;
  uint16_t *u = string_utf16(s, &len);
  if ((uint64_t)start + n > len) {
    throw_new("java/lang/StringIndexOutOfBoundsException", utf8 ? "GetStringUTFRegion"
                                                                : "GetStringRegion");
  } else if (utf8) {
    utf16_to_utf8(u + start, n, g2h(buf));
  } else {
    memcpy(g2h(buf), u + start, (size_t)n * 2);
  }
  free(u);
}

static int dispatch_env(cpu *c, uint32_t slot) {
  if (dispatch_call(c, slot) || dispatch_prim_array(c, slot)) return 1;
  uint32_t a1 = cpu_arg(c, 1), a2 = cpu_arg(c, 2);
  jobj *o;
  switch (slot) {
  case 4: cpu_ret(c, JNI_VERSION_1_6); return 1; /* GetVersion */
  case 6: find_class(c); return 1;
  case 10: { /* GetSuperclass: the fake VM's hierarchy is flat */
    int cls = class_of_handle(a1);
    int obj = class_index("java/lang/Object");
    cpu_ret(c, cls < 0 || cls == obj ? 0 : classes[obj].handle);
    return 1;
  }
  case 11: /* IsAssignableFrom(env, sub, sup) */
    cpu_ret(c, a1 == a2 || class_of_handle(a2) == class_index("java/lang/Object"));
    return 1;
  case 13: pending = a1; cpu_ret(c, 0); return 1; /* Throw */
  case 14: { /* ThrowNew(env, cls, msg) */
    int cls = class_of_handle(a1);
    throw_new(cls < 0 ? "java/lang/Throwable" : classes[cls].name, a2 ? (char *)g2h(a2) : "");
    cpu_ret(c, 0);
    return 1;
  }
  case 15: cpu_ret(c, pending); return 1; /* ExceptionOccurred */
  case 16: /* ExceptionDescribe */
    if ((o = obj_get(pending))) fprintf(stderr, "[jni] pending %s\n", classes[o->cls].name);
    return 1;
  case 17: pending = 0; return 1; /* ExceptionClear */
  case 18: /* FatalError */
    fprintf(stderr, "[jni] FatalError: %s\n", a1 ? (char *)g2h(a1) : "");
    c->faulted = 1;
    c->ops->halt(c);
    return 1;
  case 19: case 26: cpu_ret(c, 0); return 1; /* PushLocalFrame, EnsureLocalCapacity */
  case 20: case 21: case 25: case 226: /* PopLocalFrame, New{Global,Local,WeakGlobal}Ref */
    cpu_ret(c, a1);
    return 1;
  case 22: case 23: case 227: return 1; /* Delete{Global,Local,WeakGlobal}Ref: no GC */
  case 24: cpu_ret(c, a1 == a2); return 1; /* IsSameObject */
  case 27: case 28: case 29: case 30: new_object(c, slot); return 1;
  case 31: /* GetObjectClass */
    if (!(o = obj_get(a1))) {
      cpu_ret(c, 0);
    } else {
      int cls = o->kind == OBJ_CLASS ? class_index("java/lang/Class") : o->cls;
      cpu_ret(c, classes[cls].handle);
    }
    return 1;
  case 32: { /* IsInstanceOf(env, obj, cls); null is an instance of everything */
    int cls = class_of_handle(a2);
    o = obj_get(a1);
    cpu_ret(c, !o || (o->kind != OBJ_CLASS && o->cls == cls) ||
                   cls == class_index("java/lang/Object"));
    return 1;
  }
  case 33: cpu_ret(c, get_method_id(a1, a2, cpu_arg(c, 3), 0)); return 1;
  case 94: cpu_ret(c, get_field_id(a1, a2, cpu_arg(c, 3), 0)); return 1;
  case 113: cpu_ret(c, get_method_id(a1, a2, cpu_arg(c, 3), 1)); return 1;
  case 144: cpu_ret(c, get_field_id(a1, a2, cpu_arg(c, 3), 1)); return 1;
  case 163: { /* NewString(env, const jchar *, len) */
    char *s = malloc((size_t)a2 * 3 + 1);
    utf16_to_utf8(g2h(a1), a2, s);
    cpu_ret(c, jni_new_string(s));
    free(s);
    return 1;
  }
  case 167: cpu_ret(c, a1 ? jni_new_string(g2h(a1)) : 0); return 1; /* NewStringUTF */
  case 171: cpu_ret(c, (o = array_get(a1)) ? o->len : 0); return 1; /* GetArrayLength */
  case 172: { /* NewObjectArray(env, len, cls, init) */
    int cls = class_of_handle(a2);
    uint32_t h = array_new('L', cls >= 0 ? classes[cls].name : "java/lang/Object", a1);
    for (uint32_t i = 0; i < a1; i++) objs[h - OBJ_BASE].refs[i] = cpu_arg(c, 3);
    cpu_ret(c, h);
    return 1;
  }
  case 173: /* GetObjectArrayElement(env, array, index) */
    o = array_get(a1);
    if (!o || o->elem != 'L' || a2 >= o->len) {
      throw_new("java/lang/ArrayIndexOutOfBoundsException", "GetObjectArrayElement");
      cpu_ret(c, 0);
    } else {
      cpu_ret(c, o->refs[a2]);
    }
    return 1;
  case 174: /* SetObjectArrayElement(env, array, index, value) */
    if (jni_set_object_array_element(a1, a2, cpu_arg(c, 3)) != 0)
      throw_new("java/lang/ArrayIndexOutOfBoundsException", "SetObjectArrayElement");
    return 1;
  case 215: register_natives(c); return 1;
  case 216: case 217: case 218: cpu_ret(c, 0); return 1; /* UnregisterNatives, Monitor* */
  case 219: gm_w32(a1, vm_addr); cpu_ret(c, 0); return 1; /* GetJavaVM */
  case 222: /* GetPrimitiveArrayCritical(env, array, isCopy) */
    o = array_get(a1);
    if (a2) *(uint8_t *)g2h(a2) = 0;
    cpu_ret(c, o && o->elem != 'L' ? o->data : 0);
    return 1;
  case 223: return 1; /* ReleasePrimitiveArrayCritical */
  case 228: cpu_ret(c, pending != 0); return 1; /* ExceptionCheck */
  case 232: cpu_ret(c, obj_get(a1) ? 1 : 0); return 1; /* GetObjectRefType: local */
  }

  /* String functions on (env, jstring, ...). */
  if ((slot >= 164 && slot <= 170) || slot == 220 || slot == 221 || slot == 224 || slot == 225) {
    jobj *s = string_get(a1);
    if (!s) {
      fprintf(stderr, "[jni] %s on 0x%x, which is not a String\n", env_slot_names[slot], a1);
      unimplemented_calls++;
      cpu_ret(c, 0);
      return 1;
    }
    switch (slot) {
    case 164: cpu_ret(c, utf8_to_utf16(s->utf8, NULL)); return 1; /* GetStringLength */
    case 165: case 224: cpu_ret(c, string_chars_copy(c, s)); return 1;
    case 166: case 170: case 225: guest_free(a2); return 1; /* Release*: our copies */
    case 168: cpu_ret(c, (uint32_t)strlen(s->utf8)); return 1; /* GetStringUTFLength */
    case 169: /* GetStringUTFChars(env, str, isCopy) */
      if (a2) *(uint8_t *)g2h(a2) = 1;
      cpu_ret(c, guest_strdup(s->utf8));
      return 1;
    case 220: string_region(c, s, 0); return 1;
    case 221: string_region(c, s, 1); return 1;
    }
  }
  return 0;
}

static void dispatch_vm(cpu *c, uint32_t slot) {
  switch (slot) {
  case 3: case 5: cpu_ret(c, 0); return; /* DestroyJavaVM, DetachCurrentThread */
  case 4: case 6: case 7: /* AttachCurrentThread[AsDaemon](vm, env*, args), GetEnv(vm, env*, ver) */
    gm_w32(cpu_arg(c, 1), env_addr);
    cpu_ret(c, 0);
    return;
  }
  fprintf(stderr, "[jni] JavaVM->%s called (unimplemented)\n",
          vm_slot_names[slot] ? vm_slot_names[slot] : "<reserved>");
  unimplemented_calls++;
  cpu_ret(c, (uint32_t)-1); /* JNI_ERR */
}

void jni_dispatch(cpu *c, uint32_t imm) {
  uint32_t slot = imm & ~JNI_SVC_MASK;
  if ((imm & JNI_SVC_MASK) == JNI_SVC_VM) {
    if (slot < N_VM_SLOTS) dispatch_vm(c, slot);
    else unimplemented_calls++;
    return;
  }
  if (slot < N_ENV_SLOTS && dispatch_env(c, slot)) return;
  fprintf(stderr, "[jni] JNIEnv->%s called (unimplemented)\n",
          slot < N_ENV_SLOTS && env_slot_names[slot] ? env_slot_names[slot] : "<reserved>");
  unimplemented_calls++;
  cpu_ret(c, 0);
}

uint32_t jni_unimplemented_calls(void) { return unimplemented_calls; }

uint32_t jni_env(void) { return env_addr; }
uint32_t jni_vm(void) { return vm_addr; }

int jni_init(void) {
  uint32_t n = N_ENV_SLOTS + N_VM_SLOTS;
  uint32_t thunks = gm_alloc(n * 8, 0, "jni thunks");
  uint32_t tables = gm_alloc(n * 4 + 16, 0, "jni tables");
  if (!thunks || !tables) return -1;
  uint32_t env_table = tables, vm_table = tables + N_ENV_SLOTS * 4;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t imm = i < N_ENV_SLOTS ? JNI_SVC_BASE | i : JNI_SVC_VM | (i - N_ENV_SLOTS);
    gm_w32(thunks + i * 8, 0xEF000000u | imm); /* svc #imm */
    gm_w32(thunks + i * 8 + 4, 0xE12FFF1Eu);  /* bx lr */
    gm_w32(env_table + i * 4, thunks + i * 8); /* the VM table follows the env table */
  }
  /* JNIEnv and JavaVM are pointers to their function tables. */
  env_addr = vm_table + N_VM_SLOTS * 4;
  vm_addr = env_addr + 4;
  gm_w32(env_addr, env_table);
  gm_w32(vm_addr, vm_table);
  class_index("java/lang/Object");
  class_index("java/lang/Class");
  class_index("java/lang/String");
  return 0;
}
