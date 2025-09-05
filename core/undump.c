/* ============================================================================
   undump.c — Chargeur d’images/bytecode VT (C17, portable)
   - Format conteneur binaire "VTBC"
   - Validation stricte: bornes, endianness, CRC-32
   - Sections supportées: "CODE","KCON","STRS","SYMS","FUNC","DBG\0"
   - API minimale: vt_img_load_file / vt_img_load_memory / vt_img_release
                   vt_img_find / vt_img_info
   - Zéro dépendance externe. Licence: MIT.
   ============================================================================
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------------------
   Définition interne de l’image.
   Une .h publique peut redéclarer ce type si besoin.
---------------------------------------------------------------------------- */
typedef struct vt_img {
  uint8_t* buf; /* tampon propriétaire si owns=1 */
  size_t size;
  int owns;

  /* Sections mappées après parse */
  const uint8_t* code;
  size_t code_sz;
  const uint8_t* kcon;
  size_t kcon_sz;
  const uint8_t* strs;
  size_t strs_sz;
  const uint8_t* syms;
  size_t syms_sz;
  const uint8_t* func;
  size_t func_sz;
  const uint8_t* dbg;
  size_t dbg_sz;

  /* Métadonnées header */
  uint16_t ver_major, ver_minor;
  uint16_t flags;       /* bit0: little-endian, bit1: big-endian */
  uint32_t header_size; /* octets jusqu’au début TOC */
  uint64_t image_size;  /* taille totale attendue */
  uint32_t crc32_file;  /* CRC stocké */
  uint32_t toc_count;
} vt_img;

/* ----------------------------------------------------------------------------
   Helpers sécurisés
---------------------------------------------------------------------------- */
#define VT_MIN(a, b) ((a) < (b) ? (a) : (b))
#define VT_MAX(a, b) ((a) > (b) ? (a) : (b))

static int vt__is_le(void) {
  uint16_t x = 1;
  return *(uint8_t*)&x;
}

static int vt__check_bounds(size_t base, size_t need, size_t cap) {
  if (need > cap) return 0;
  if (base > cap - need) return 0;
  return 1;
}

static uint16_t vt__rd_u16_le(const uint8_t* p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t vt__rd_u32_le(const uint8_t* p) {
  return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
static uint64_t vt__rd_u64_le(const uint8_t* p) {
  return (uint64_t)vt__rd_u32_le(p) | ((uint64_t)vt__rd_u32_le(p + 4) << 32);
}

/* ----------------------------------------------------------------------------
   CRC-32 (poly 0xEDB88320), table générée au premier appel
---------------------------------------------------------------------------- */
static uint32_t vt__crc_table[256];
static int vt__crc_ready = 0;

static void vt__crc_init(void) {
  if (vt__crc_ready) return;
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    vt__crc_table[i] = c;
  }
  vt__crc_ready = 1;
}
static uint32_t vt__crc32(const void* data, size_t len) {
  vt__crc_init();
  const uint8_t* p = (const uint8_t*)data;
  uint32_t c = ~0u;
  for (size_t i = 0; i < len; i++)
    c = vt__crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

/* ----------------------------------------------------------------------------
   Format binaire "VTBC" (little-endian)
   Layout:
     struct FileHeader {
       char     magic[4]      = "VTBC";
       u16      ver_major;    // ex: 1
       u16      ver_minor;    // ex: 0
       u16      flags;        // bit0=LE, bit1=BE, autres réservés
       u16      _pad;         // 0
       u32      header_size;  // octets du header total (incluant ce champ)
       u64      image_size;   // taille totale du fichier
       u32      crc32;        // CRC-32 du payload à partir de `header_size`
jusqu’à fin u32      toc_count;    // nb d’entrées TOC
       // suivi de toc_count entrées:
       // struct TocEntry {
       //   char tag[4];   // "CODE","KCON","STRS","SYMS","FUNC","DBG\0"
       //   u32  _zero;
       //   u64  offset;   // depuis début fichier
       //   u64  size;
       // }
     }
---------------------------------------------------------------------------- */
typedef struct {
  char magic[4];
  uint16_t ver_major, ver_minor;
  uint16_t flags, pad;
  uint32_t header_size;
  uint64_t image_size;
  uint32_t crc32;
  uint32_t toc_count;
} vt__FileHeader;

typedef struct {
  char tag[4];
  uint32_t zero;
  uint64_t offset;
  uint64_t size;
} vt__TocEntry;

/* ----------------------------------------------------------------------------
   Lecture de fichier
---------------------------------------------------------------------------- */
static int vt__read_file(const char* path, uint8_t** out_buf, size_t* out_sz) {
  *out_buf = NULL;
  *out_sz = 0;
  FILE* f = fopen(path, "rb");
  if (!f) return -errno;
  if (fseek(f, 0, SEEK_END) != 0) {
    int e = -errno;
    fclose(f);
    return e;
  }
  long ls = ftell(f);
  if (ls < 0) {
    int e = -errno;
    fclose(f);
    return e;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    int e = -errno;
    fclose(f);
    return e;
  }
  size_t sz = (size_t)ls;
  uint8_t* b = (uint8_t*)malloc(sz ? sz : 1);
  if (!b) {
    fclose(f);
    return -ENOMEM;
  }
  size_t rd = fread(b, 1, sz, f);
  fclose(f);
  if (rd != sz) {
    free(b);
    return -EIO;
  }
  *out_buf = b;
  *out_sz = sz;
  return 0;
}

/* ----------------------------------------------------------------------------
   Parse header + TOC, map sections
---------------------------------------------------------------------------- */
static int vt__parse(vt_img* img) {
  if (!img || !img->buf || img->size < sizeof(vt__FileHeader)) return -EINVAL;

  const uint8_t* p = img->buf;
  vt__FileHeader h;
  memcpy(&h, p, sizeof h); /* lecture brute puis interprétation LE */

  if (memcmp(h.magic, "VTBC", 4) != 0) return -EINVAL;

  /* Tous les champs header sont little-endian dans le fichier */
  img->ver_major = vt__rd_u16_le((const uint8_t*)&h.ver_major);
  img->ver_minor = vt__rd_u16_le((const uint8_t*)&h.ver_minor);
  img->flags = vt__rd_u16_le((const uint8_t*)&h.flags);
  img->header_size = vt__rd_u32_le((const uint8_t*)&h.header_size);
  img->image_size = vt__rd_u64_le((const uint8_t*)&h.image_size);
  img->crc32_file = vt__rd_u32_le((const uint8_t*)&h.crc32);
  img->toc_count = vt__rd_u32_le((const uint8_t*)&h.toc_count);

  /* Sanity */
  if (img->header_size < sizeof(vt__FileHeader)) return -EINVAL;
  if (img->image_size != (uint64_t)img->size) return -EINVAL;
  if (!vt__check_bounds(0, img->header_size, img->size)) return -EINVAL;

  /* CRC du payload (après header) */
  const uint8_t* payload = img->buf + img->header_size;
  size_t payload_sz = img->size - img->header_size;
  uint32_t crc_calc = vt__crc32(payload, payload_sz);
  if (crc_calc != img->crc32_file) return -EBADMSG;

  /* TOC: placé immédiatement après le FileHeader, jusqu’à header_size */
  size_t toc_bytes = img->header_size - sizeof(vt__FileHeader);
  if (toc_bytes / sizeof(vt__TocEntry) < img->toc_count) return -EINVAL;

  const vt__TocEntry* toc =
      (const vt__TocEntry*)(img->buf + sizeof(vt__FileHeader));

  /* Reset sections */
  img->code = img->kcon = img->strs = img->syms = img->func = img->dbg = NULL;
  img->code_sz = img->kcon_sz = img->strs_sz = img->syms_sz = img->func_sz =
      img->dbg_sz = 0;

  /* Map */
  for (uint32_t i = 0; i < img->toc_count; i++) {
    /* Lire LE */
    char tag[4];
    memcpy(tag, toc[i].tag, 4);
    uint64_t off = vt__rd_u64_le((const uint8_t*)&toc[i].offset);
    uint64_t sz = vt__rd_u64_le((const uint8_t*)&toc[i].size);
    if (sz > (uint64_t)SIZE_MAX) return -EOVERFLOW;
    if (!vt__check_bounds(0, (size_t)off, img->size)) return -EINVAL;
    if (!vt__check_bounds((size_t)off, (size_t)sz, img->size)) return -EINVAL;

    const uint8_t* base = img->buf + (size_t)off;
    if (memcmp(tag, "CODE", 4) == 0) {
      img->code = base;
      img->code_sz = (size_t)sz;
      continue;
    }
    if (memcmp(tag, "KCON", 4) == 0) {
      img->kcon = base;
      img->kcon_sz = (size_t)sz;
      continue;
    }
    if (memcmp(tag, "STRS", 4) == 0) {
      img->strs = base;
      img->strs_sz = (size_t)sz;
      continue;
    }
    if (memcmp(tag, "SYMS", 4) == 0) {
      img->syms = base;
      img->syms_sz = (size_t)sz;
      continue;
    }
    if (memcmp(tag, "FUNC", 4) == 0) {
      img->func = base;
      img->func_sz = (size_t)sz;
      continue;
    }
    if (memcmp(tag, "DBG\0", 4) == 0) {
      img->dbg = base;
      img->dbg_sz = (size_t)sz;
      continue;
    }
    /* Tags inconnus: tolérés, ignorés */
  }

  /* Sections minimales requises */
  if (!img->code || !img->func) return -ENOEXEC;
  return 0;
}

/* ----------------------------------------------------------------------------
   API publique basique
---------------------------------------------------------------------------- */
int vt_img_load_memory(const void* data, size_t size, int copy,
                       vt_img** out_img) {
  if (!data || !size || !out_img) return -EINVAL;
  vt_img* img = (vt_img*)calloc(1, sizeof *img);
  if (!img) return -ENOMEM;

  if (copy) {
    img->buf = (uint8_t*)malloc(size);
    if (!img->buf) {
      free(img);
      return -ENOMEM;
    }
    memcpy(img->buf, data, size);
    img->size = size;
    img->owns = 1;
  } else {
    img->buf = (uint8_t*)(uintptr_t)data;
    img->size = size;
    img->owns = 0;
  }

  int rc = vt__parse(img);
  if (rc != 0) {
    if (img->owns) free(img->buf);
    free(img);
    return rc;
  }
  *out_img = img;
  return 0;
}

int vt_img_load_file(const char* path, vt_img** out_img) {
  if (!path || !out_img) return -EINVAL;
  uint8_t* b = NULL;
  size_t sz = 0;
  int rc = vt__read_file(path, &b, &sz);
  if (rc != 0) return rc;

  vt_img* img = NULL;
  rc = vt_img_load_memory(b, sz, /*copy=*/0, &img);
  if (rc != 0) {
    free(b);
    return rc;
  }
  /* On doit devenir propriétaire du buffer puisque copy=0 */
  img->owns = 1;
  img->buf = b;
  *out_img = img;
  return 0;
}

void vt_img_release(vt_img* img) {
  if (!img) return;
  if (img->owns && img->buf) free(img->buf);
  free(img);
}

/* Cherche une section par tag 4-char. Renvoie 0 si trouvée. */
int vt_img_find(const vt_img* img, const char tag4[4], const uint8_t** out_ptr,
                size_t* out_sz) {
  if (!img || !tag4 || !out_ptr || !out_sz) return -EINVAL;
  if (memcmp(tag4, "CODE", 4) == 0 && img->code) {
    *out_ptr = img->code;
    *out_sz = img->code_sz;
    return 0;
  } else if (memcmp(tag4, "KCON", 4) == 0 && img->kcon) {
    *out_ptr = img->kcon;
    *out_sz = img->kcon_sz;
    return 0;
  } else if (memcmp(tag4, "STRS", 4) == 0 && img->strs) {
    *out_ptr = img->strs;
    *out_sz = img->strs_sz;
    return 0;
  } else if (memcmp(tag4, "SYMS", 4) == 0 && img->syms) {
    *out_ptr = img->syms;
    *out_sz = img->syms_sz;
    return 0;
  } else if (memcmp(tag4, "FUNC", 4) == 0 && img->func) {
    *out_ptr = img->func;
    *out_sz = img->func_sz;
    return 0;
  } else if (memcmp(tag4, "DBG\0", 4) == 0 && img->dbg) {
    *out_ptr = img->dbg;
    *out_sz = img->dbg_sz;
    return 0;
  }
  return -ENOENT;
}

/* Dump texte des métadonnées (pour debug CLI). */
void vt_img_info(const vt_img* img, FILE* out) {
  if (!img) return;
  if (!out) out = stderr;
  fprintf(out, "VTBC image: %zu bytes  ver=%u.%u  flags=0x%04x  toc=%u\n",
          img->size, img->ver_major, img->ver_minor, img->flags,
          img->toc_count);
  fprintf(out, " sections:\n");
  fprintf(out, "  CODE: %10zu @ %p\n", img->code_sz, (void*)img->code);
  fprintf(out, "  KCON: %10zu @ %p\n", img->kcon_sz, (void*)img->kcon);
  fprintf(out, "  STRS: %10zu @ %p\n", img->strs_sz, (void*)img->strs);
  fprintf(out, "  SYMS: %10zu @ %p\n", img->syms_sz, (void*)img->syms);
  fprintf(out, "  FUNC: %10zu @ %p\n", img->func_sz, (void*)img->func);
  fprintf(out, "  DBG : %10zu @ %p\n", img->dbg_sz, (void*)img->dbg);
}

/* ----------------------------------------------------------------------------
   Optionnel: itérateur de strings (STRS=blob 0-terminé concaténé)
---------------------------------------------------------------------------- */
typedef struct {
  const uint8_t* cur;
  const uint8_t* end;
} vt_strs_it;

void vt_strs_begin(const vt_img* img, vt_strs_it* it) {
  if (!it) {
    return;
  }
  if (!img || !img->strs) {
    it->cur = NULL;
    it->end = NULL;
    return;
  }
  it->cur = img->strs;
  it->end = img->strs + img->strs_sz;
}
const char* vt_strs_next(vt_strs_it* it) {
  if (!it || !it->cur || it->cur >= it->end) return NULL;
  const char* s = (const char*)it->cur;
  size_t left = (size_t)(it->end - it->cur);
  size_t n = strnlen(s, left);
  if (n == left) { /* pas de NUL */
    it->cur = it->end;
    return NULL;
  }
  it->cur += n + 1;
  return s;
}

/* ----------------------------------------------------------------------------
   Stubs d’export minimal si un .h formel n’est pas encore écrit
   (Expose symboles utilisables ailleurs)
---------------------------------------------------------------------------- */
#ifdef _MSC_VER
#define VT_EXPORT __declspec(dllexport)
#else
#define VT_EXPORT
#endif

VT_EXPORT int vt_undump_load_file(const char* path, void** out_handle) {
  vt_img* img = NULL;
  int rc = vt_img_load_file(path, &img);
  if (rc != 0) return rc;
  *out_handle = img;
  return 0;
}
VT_EXPORT void vt_undump_release(void* handle) {
  vt_img_release((vt_img*)handle);
}
VT_EXPORT int vt_undump_find(void* handle, const char tag4[4],
                             const void** out_ptr, size_t* out_sz) {
  return vt_img_find((const vt_img*)handle, tag4, (const uint8_t**)out_ptr,
                     out_sz);
}
VT_EXPORT void vt_undump_info(void* handle, FILE* out) {
  vt_img_info((const vt_img*)handle, out);
}
