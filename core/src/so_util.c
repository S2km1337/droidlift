#include "so_util.h"

#include <stdlib.h>
#include <string.h>

#include "dynlib.h"
#include "elf32.h"
#include "guest_mem.h"

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = len > 0 ? malloc((size_t)len) : NULL;
  if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  *size = (size_t)len;
  return buf;
}

static const Elf32_Sym *sym_at(const so_module *m, uint32_t index) {
  return (const Elf32_Sym *)g2h(m->symtab + index * sizeof(Elf32_Sym));
}

static const char *str_at(const so_module *m, uint32_t offset) {
  return (const char *)g2h(m->strtab + offset);
}

#define SO_MAX_MODULES 8

static so_module *all_modules[SO_MAX_MODULES];
static int n_modules;

const so_module *so_module_containing(uint32_t addr) {
  for (int i = 0; i < n_modules; i++)
    if (addr - all_modules[i]->base < all_modules[i]->size) return all_modules[i];
  return NULL;
}

const so_module *so_module_find(const char *soname) {
  for (int i = 0; i < n_modules; i++)
    if (all_modules[i]->soname && strcmp(all_modules[i]->soname, soname) == 0) return all_modules[i];
  return NULL;
}

int so_load(so_module *m, const char *path) {
  memset(m, 0, sizeof(*m));
  int known = 0;
  for (int i = 0; i < n_modules; i++) known |= all_modules[i] == m;
  if (!known && n_modules < SO_MAX_MODULES) all_modules[n_modules++] = m;
  snprintf(m->path, sizeof(m->path), "%s", path);

  size_t file_size;
  uint8_t *file = read_file(path, &file_size);
  if (!file) {
    fprintf(stderr, "so_load: cannot read %s\n", path);
    return -1;
  }

  int ret = -1;
  const Elf32_Ehdr *eh = (const Elf32_Ehdr *)file;
  if (file_size < sizeof(*eh) || memcmp(eh->e_ident, "\177ELF", 4) != 0 ||
      eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB ||
      eh->e_type != ET_DYN || eh->e_machine != EM_ARM ||
      eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf32_Phdr) > file_size) {
    fprintf(stderr, "so_load: %s is not a 32-bit little-endian ARM shared object\n", path);
    goto out;
  }
  const Elf32_Phdr *ph = (const Elf32_Phdr *)(file + eh->e_phoff);

  /* The libraries are linked at vaddr 0; map the whole span in one region. */
  uint32_t span = 0;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > span)
      span = ph[i].p_vaddr + ph[i].p_memsz;
  const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
  m->size = span;
  m->base = gm_alloc(span, 0x100000, strdup(name));
  if (!m->base) goto out;

  uint32_t dynamic = 0;
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type == PT_LOAD) {
      if (ph[i].p_offset + (size_t)ph[i].p_filesz > file_size || ph[i].p_filesz > ph[i].p_memsz) {
        fprintf(stderr, "so_load: %s: bad PT_LOAD %d\n", path, i);
        goto out;
      }
      memcpy(g2h(m->base + ph[i].p_vaddr), file + ph[i].p_offset, ph[i].p_filesz);
    } else if (ph[i].p_type == PT_DYNAMIC) {
      dynamic = m->base + ph[i].p_vaddr;
    } else if (ph[i].p_type == PT_ARM_EXIDX) {
      m->exidx = m->base + ph[i].p_vaddr;
      m->exidx_count = ph[i].p_memsz / 8;
    }
  }
  if (!dynamic) {
    fprintf(stderr, "so_load: %s has no PT_DYNAMIC\n", path);
    goto out;
  }

  uint32_t needed_off[SO_MAX_NEEDED], soname_off = 0, hash = 0;
  for (const Elf32_Dyn *d = g2h(dynamic); d->d_tag != DT_NULL; d++) {
    switch (d->d_tag) {
    case DT_NEEDED:
      if (m->n_needed < SO_MAX_NEEDED) needed_off[m->n_needed++] = d->d_val;
      break;
    case DT_SONAME: soname_off = d->d_val; break;
    case DT_HASH: hash = m->base + d->d_val; break;
    case DT_STRTAB: m->strtab = m->base + d->d_val; break;
    case DT_SYMTAB: m->symtab = m->base + d->d_val; break;
    case DT_REL: m->rel = m->base + d->d_val; break;
    case DT_RELSZ: m->relsz = d->d_val; break;
    case DT_JMPREL: m->jmprel = m->base + d->d_val; break;
    case DT_PLTRELSZ: m->pltrelsz = d->d_val; break;
    case DT_INIT_ARRAY: m->init_array = m->base + d->d_val; break;
    case DT_INIT_ARRAYSZ: m->init_array_count = d->d_val / 4; break;
    case DT_FINI_ARRAY: m->fini_array = m->base + d->d_val; break;
    case DT_FINI_ARRAYSZ: m->fini_array_count = d->d_val / 4; break;
    }
  }
  if (!m->strtab || !m->symtab || !hash) {
    fprintf(stderr, "so_load: %s lacks DT_STRTAB, DT_SYMTAB or DT_HASH\n", path);
    goto out;
  }
  m->nbucket = gm_r32(hash);
  m->nsyms = gm_r32(hash + 4);
  m->buckets = hash + 8;
  m->chains = m->buckets + m->nbucket * 4;
  for (int i = 0; i < m->n_needed; i++) m->needed[i] = str_at(m, needed_off[i]);
  m->soname = soname_off ? str_at(m, soname_off) : name;
  ret = 0;

out:
  free(file);
  return ret;
}

static uint32_t elf_hash(const char *name) {
  uint32_t h = 0, g;
  while (*name) {
    h = (h << 4) + (uint8_t)*name++;
    g = h & 0xf0000000u;
    if (g) h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

uint32_t so_find_export(const so_module *m, const char *name) {
  if (!m->nbucket) return 0;
  uint32_t i = gm_r32(m->buckets + (elf_hash(name) % m->nbucket) * 4);
  for (; i != 0 && i < m->nsyms; i = gm_r32(m->chains + i * 4)) {
    const Elf32_Sym *s = sym_at(m, i);
    int bind = ELF32_ST_BIND(s->st_info);
    if (s->st_shndx != SHN_UNDEF && (bind == STB_GLOBAL || bind == STB_WEAK) &&
        strcmp(str_at(m, s->st_name), name) == 0)
      return m->base + s->st_value;
  }
  return 0;
}

static const so_module *find_loaded(so_module *const *loaded, int n, const char *soname) {
  for (int i = 0; i < n; i++)
    if (strcmp(loaded[i]->soname, soname) == 0) return loaded[i];
  return NULL;
}

/*
 * Symbol bindings, cached per symbol index so each import is resolved and
 * reported once. Lookup order follows the old bionic linker: the module
 * itself, then its DT_NEEDED libraries in order, then the host (dynlib).
 */
typedef struct {
  uint8_t done, ok;
  uint32_t addr;
} binding;

static int resolve(so_module *m, so_module *const *loaded, int n_loaded, uint32_t index,
                   binding *b, FILE *report) {
  if (b->done) return b->ok;
  b->done = 1;
  const Elf32_Sym *s = sym_at(m, index);
  const char *name = str_at(m, s->st_name);

  if (s->st_shndx != SHN_UNDEF) {
    b->addr = m->base + s->st_value;
    return b->ok = 1;
  }

  m->stats.imports++;
  for (int i = 0; i < m->n_needed; i++) {
    const so_module *dep = find_loaded(loaded, n_loaded, m->needed[i]);
    uint32_t addr = dep ? so_find_export(dep, name) : 0;
    if (addr) {
      m->stats.imports_lib++;
      if (report) fprintf(report, "%s\t%s\t%s\t0x%08x\n", m->soname, name, dep->soname, addr);
      b->addr = addr;
      return b->ok = 1;
    }
  }

  const dynlib_entry *e = dynlib_find(name);
  if (!e) {
    m->stats.unresolved++;
    if (report) fprintf(report, "%s\t%s\tUNRESOLVED\t-\n", m->soname, name);
    fprintf(stderr, "so_relocate: %s: unresolved import %s\n", m->soname, name);
    return b->ok = 0;
  }
  static const char *kind[] = {"stub", "data", "null"};
  if (e->kind == DL_STUB) m->stats.imports_stub++;
  if (e->kind == DL_DATA) m->stats.imports_data++;
  if (e->kind == DL_WEAK_NULL) m->stats.imports_null++;
  if (report)
    fprintf(report, "%s\t%s\t%s:%s\t0x%08x\n", m->soname, name, kind[e->kind], e->lib, e->addr);
  b->addr = e->addr;
  return b->ok = 1;
}

static int apply(so_module *m, so_module *const *loaded, int n_loaded, uint32_t table,
                 uint32_t size, binding *bindings, FILE *report) {
  int ok = 1;
  for (uint32_t off = 0; off + sizeof(Elf32_Rel) <= size; off += sizeof(Elf32_Rel)) {
    const Elf32_Rel *r = g2h(table + off);
    uint32_t where = m->base + r->r_offset;
    uint32_t type = ELF32_R_TYPE(r->r_info), sym = ELF32_R_SYM(r->r_info);
    if (r->r_offset + 4 > m->size) {
      fprintf(stderr, "so_relocate: %s: relocation at 0x%x outside the image\n", m->soname,
              r->r_offset);
      m->stats.unsupported++;
      ok = 0;
      continue;
    }
    if (type == R_ARM_RELATIVE) {
      gm_w32(where, gm_r32(where) + m->base);
      m->stats.relative++;
      continue;
    }
    if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) {
      fprintf(stderr, "so_relocate: %s: unsupported relocation type %u at 0x%x\n", m->soname,
              type, r->r_offset);
      m->stats.unsupported++;
      ok = 0;
      continue;
    }
    if (sym >= m->nsyms || !resolve(m, loaded, n_loaded, sym, &bindings[sym], report)) {
      ok = 0;
      continue;
    }
    if (type == R_ARM_ABS32) {
      gm_w32(where, gm_r32(where) + bindings[sym].addr);
      m->stats.abs32++;
    } else {
      gm_w32(where, bindings[sym].addr);
      if (type == R_ARM_GLOB_DAT) m->stats.glob_dat++;
      else m->stats.jump_slot++;
    }
  }
  return ok;
}

int so_relocate(so_module *m, so_module *const *loaded, int n_loaded, FILE *report) {
  binding *bindings = calloc(m->nsyms ? m->nsyms : 1, sizeof(binding));
  if (!bindings) return -1;
  int ok = apply(m, loaded, n_loaded, m->rel, m->relsz, bindings, report);
  ok &= apply(m, loaded, n_loaded, m->jmprel, m->pltrelsz, bindings, report);
  /* Bind imports no relocation mentions too, so every import is accounted for. */
  for (uint32_t i = 1; i < m->nsyms; i++) {
    const Elf32_Sym *s = sym_at(m, i);
    if (s->st_shndx == SHN_UNDEF && s->st_name && *str_at(m, s->st_name))
      ok &= resolve(m, loaded, n_loaded, i, &bindings[i], report);
  }
  free(bindings);
  return ok ? 0 : -1;
}
