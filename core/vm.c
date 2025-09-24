// SPDX-License-Identifier: MIT
/* ============================================================================
   vm.c — Implémentation minimale de la VM VitteLight (stub fonctionnel)

   Cette version assure la cohérence avec l’API exposée par vm.h sans encore
   proposer une exécution de bytecode. Les hooks principaux sont en place et
   retournent ENOSYS lorsque la fonctionnalité n’est pas disponible.
   ============================================================================ */

#include "vm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* VM interne                                                                 */
/* -------------------------------------------------------------------------- */
struct vt_vm {
  vt_vm_config cfg;
  vt_cfunc*    natives;
  size_t       native_cap;
};

static const vt_vm_config VT_VM_DEFAULT_CFG = {
  .initial_stack_cap   = 256,
  .initial_frame_cap   = 32,
  .default_step_limit  = 0,
  .enable_traces       = 0,
};

static int vt__ensure_native_cap(vt_vm* vm, size_t need) {
  if (need <= vm->native_cap) return 0;
  size_t cap = vm->native_cap ? vm->native_cap : 16;
  while (cap <= need) cap *= 2;
  vt_cfunc* tmp = (vt_cfunc*)realloc(vm->natives, cap * sizeof(*vm->natives));
  if (!tmp) return -1;
  memset(tmp + vm->native_cap, 0, (cap - vm->native_cap) * sizeof(*vm->natives));
  vm->natives    = tmp;
  vm->native_cap = cap;
  return 0;
}

/* -------------------------------------------------------------------------- */
/* API publique                                                                */
/* -------------------------------------------------------------------------- */
vt_vm* vt_vm_new(const vt_vm_config* cfg) {
  vt_vm* vm = (vt_vm*)calloc(1, sizeof(vt_vm));
  if (!vm) return NULL;
  vm->cfg = cfg ? *cfg : VT_VM_DEFAULT_CFG;
  vm->natives = NULL;
  vm->native_cap = 0;
  return vm;
}

void vt_vm_free(vt_vm* vm) {
  if (!vm) return;
  free(vm->natives);
  free(vm);
}

int vt_vm_set_native(vt_vm* vm, uint16_t symbol_id, vt_cfunc fn) {
  if (!vm) return -EINVAL;
  if (vt__ensure_native_cap(vm, (size_t)symbol_id + 1) != 0) return -ENOMEM;
  vm->natives[symbol_id] = fn;
  return 0;
}

vt_value vt_make_native(vt_cfunc fn) {
#ifdef VT_OBJECT_H
  vt_value val;
  val.type  = VT_PTR;
  val.flags = 0;
  val.as.p  = (void*)fn;
  return val;
#else
  vt_value val;
  memset(&val, 0, sizeof val);
  val.t = VT_T_NATIVE;
  vt_value_set_ptr(val, (void*)fn);
  return val;
#endif
}

int vt_vm_load_image(vt_vm* vm, const char* path) {
  (void)vm;
  (void)path;
  return -ENOSYS;
}

int vt_vm_run(vt_vm* vm, uint64_t step_limit) {
  (void)vm;
  (void)step_limit;
  return -ENOSYS;
}
