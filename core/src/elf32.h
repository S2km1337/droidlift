/* Minimal ELF32/ARM definitions (macOS has no <elf.h>). */
#ifndef ELF32_H
#define ELF32_H

#include <stdint.h>

#define EI_NIDENT 16
#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define ET_DYN 3
#define EM_ARM 40

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_ARM_EXIDX 0x70000001

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_REL 17
#define DT_RELSZ 18
#define DT_JMPREL 23
#define DT_SONAME 14
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28

#define SHN_UNDEF 0
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xf)
#define ELF32_R_SYM(i) ((i) >> 8)
#define ELF32_R_TYPE(i) ((uint8_t)(i))

#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23

typedef struct {
  uint8_t e_ident[EI_NIDENT];
  uint16_t e_type, e_machine;
  uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;

typedef struct {
  uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr;

typedef struct {
  int32_t d_tag;
  uint32_t d_val;
} Elf32_Dyn;

typedef struct {
  uint32_t st_name, st_value, st_size;
  uint8_t st_info, st_other;
  uint16_t st_shndx;
} Elf32_Sym;

typedef struct {
  uint32_t r_offset, r_info;
} Elf32_Rel;

#endif
