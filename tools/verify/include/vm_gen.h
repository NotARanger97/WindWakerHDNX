/* Included by the extracted recompiled functions (mkunit.py output) instead of funcs.h.
 * Redirects every guest memory access of ppc.h to the harness memory model (vm.h). */
#pragma once
#include "vm.h"

#undef PPC_ENTER
#define PPC_ENTER(a) ((void)0)

static inline double vm_ldf32(uint32_t ea) { return (double)u32_as_f32(vm_ld32(ea)); }
static inline double vm_ldf64(uint32_t ea) { return u64_as_f64(vm_ld64(ea)); }
static inline void vm_stf32(uint32_t ea, double d) { vm_st32(ea, f32_as_u32((float)d)); }
static inline void vm_stf64(uint32_t ea, double d) { vm_st64(ea, f64_as_u64(d)); }

static inline void vm_psq_load(Cpu* c, int fd, uint32_t ea, int w, int i) {
    uint32_t g = c->gqr[i], type = (g >> 16) & 7, scale = (g >> 24) & 0x3F;
    int sz = (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4;
    uint32_t d0 = sz == 1 ? vm_ld8(ea) : sz == 2 ? vm_ld16(ea) : vm_ld32(ea);
    c->f[fd].ps0 = psq_dequant(d0, type, scale);
    if (w) c->f[fd].ps1 = 1.0;
    else {
        uint32_t d1 = sz == 1 ? vm_ld8(ea + 1) : sz == 2 ? vm_ld16(ea + 2) : vm_ld32(ea + 4);
        c->f[fd].ps1 = psq_dequant(d1, type, scale);
    }
}
static inline void vm_psq_store(Cpu* c, int fs, uint32_t ea, int w, int i) {
    uint32_t g = c->gqr[i], type = g & 7, scale = (g >> 8) & 0x3F;
    int sz = (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4;
    uint32_t d0 = psq_quant((float)c->f[fs].ps0, type, scale);
    if (sz == 1) vm_st8(ea, d0); else if (sz == 2) vm_st16(ea, d0); else vm_st32(ea, d0);
    if (!w) {
        uint32_t d1 = psq_quant((float)c->f[fs].ps1, type, scale);
        if (sz == 1) vm_st8(ea + 1, d1); else if (sz == 2) vm_st16(ea + 2, d1); else vm_st32(ea + 4, d1);
    }
}
static inline uint32_t vm_lwarx(Cpu* c, uint32_t ea) { c->res_addr = ea; c->res_val = vm_ld32(ea); return c->res_val; }
static inline void vm_stwcx(Cpu* c, uint32_t ea, uint32_t v) {
    int ok = c->res_addr == ea && vm_ld32(ea) == c->res_val;
    if (ok) vm_st32(ea, v);
    c->res_addr = 0xFFFFFFFFu;
    c->cr[0] = 0; c->cr[1] = 0; c->cr[2] = (uint8_t)ok; c->cr[3] = c->xer_so;
}
static inline void vm_dcbz(uint32_t ea) { ea &= ~31u; for (int i = 0; i < 32; i += 4) vm_st32(ea + i, 0); }
static inline void vm_dispatch(Cpu* c) { vm_call(c, c->pc, VM_CALL_INDIRECT); }

#define ld8 vm_ld8
#define ld16 vm_ld16
#define ld32 vm_ld32
#define ld64 vm_ld64
#define st8 vm_st8
#define st16 vm_st16
#define st32 vm_st32
#define st64 vm_st64
#define ldf32 vm_ldf32
#define ldf64 vm_ldf64
#define stf32 vm_stf32
#define stf64 vm_stf64
#define psq_load vm_psq_load
#define psq_store vm_psq_store
#define ppc_lwarx vm_lwarx
#define ppc_stwcx vm_stwcx
#define ppc_dcbz vm_dcbz
#define ppc_dispatch vm_dispatch
#define ppc_dispatch_cached(c, cache) vm_dispatch(c)
#define ppc_unimplemented vm_trap
#define ppc_trap(c, a) vm_trap(c, a, 0)
#define VM_COV(f, b) vm_cov(f, b)
