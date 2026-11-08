/*
 * ELF32/ARM shared-object loader for the guest address space, modelled on
 * leos-fortune-vita's so_util: so_load maps the segments, so_relocate applies
 * the relocations and binds every import.
 */
#ifndef SO_UTIL_H
#define SO_UTIL_H

#include <stdint.h>
#include <stdio.h>

#define SO_MAX_NEEDED 16

typedef struct so_stats {
  uint32_t relative, abs32, glob_dat, jump_slot;
  uint32_t imports;        /* distinct undefined symbols referenced by relocations */
  uint32_t imports_lib;    /* bound to another loaded library */
  uint32_t imports_stub;   /* bound to a dynlib thunk */
  uint32_t imports_data;   /* bound to a dynlib data block */
  uint32_t imports_null;   /* weak, bound to 0 */
  uint32_t unresolved;     /* imports with no binding at all */
  uint32_t unsupported;    /* relocations of an unknown type */
} so_stats;

typedef struct so_module {
  char path[1024];
  const char *soname; /* points into guest memory */
  uint32_t base, size;
  uint32_t symtab, strtab, nsyms;
  uint32_t nbucket, buckets, chains;
  uint32_t rel, relsz, jmprel, pltrelsz;
  uint32_t init_array, init_array_count, fini_array, fini_array_count;
  uint32_t exidx, exidx_count;
  const char *needed[SO_MAX_NEEDED];
  int n_needed;
  so_stats stats;
} so_module;

int so_load(so_module *m, const char *path);

/* Resolves imports against `loaded` (searched in m's DT_NEEDED order), then
 * the dynlib table. Writes one line per import to `report` if non-NULL. */
int so_relocate(so_module *m, so_module *const *loaded, int n_loaded, FILE *report);

/* Guest address of an exported (defined, global or weak) symbol, or 0. */
uint32_t so_find_export(const so_module *m, const char *name);

/* The loaded module whose mapping contains addr, or NULL. */
const so_module *so_module_containing(uint32_t addr);

/* The loaded module with this DT_SONAME, or NULL. */
const so_module *so_module_find(const char *soname);

#endif
