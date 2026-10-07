/* Guest memory base on the Switch. Defined here without const (ppc.h declares it
 * const for the readers); this file must not include ppc.h. */
#include <stdint.h>

uint8_t* g_ppc_mem_base;

void switch_set_mem_base(uint8_t* base) { g_ppc_mem_base = base; }

/* libnx has no pthread_detach backend (newlib's pthread_detach returns ENOSYS and
 * std::thread::detach throws). Detached threads simply keep their handle. */
struct __pthread_t;
int __syscall_thread_detach(struct __pthread_t* thread) {
    (void)thread;
    return 0;
}
