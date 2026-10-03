#ifdef DEF_DIAG_BUILD
// Diagnostic builds only (Makefile: -Wl,--wrap=__system_allocateHeaps when DIAG=1). Restores the heap split of the
// bundled libctru fork that upstream builds run with; libctru 2.7.0 then maps the heaps with these sizes.
// Runs from __libctru_init before libc/C++ initialisation: no allocation, no libc calls.
#include <3ds.h>
#include "system/heap_split.hpp"

extern "C" {
extern u32 __ctru_heap_size;        // libctru allocateHeaps.o (weak, 0 = let allocateHeaps decide)
extern u32 __ctru_linear_heap_size; // libctru allocateHeaps.o (weak, 0 = let allocateHeaps decide)
void __real___system_allocateHeaps(void);
void __wrap___system_allocateHeaps(void) {
	Handle reslimit = 0;
	if (__ctru_heap_size == 0 && __ctru_linear_heap_size == 0 &&
	    R_SUCCEEDED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) {
		ResourceLimitType type = RESLIMIT_COMMIT;
		s64 max_commit = 0, cur_commit = 0;
		Result rc = svcGetResourceLimitLimitValues(&max_commit, reslimit, &type, 1);
		if (R_SUCCEEDED(rc)) {
			rc = svcGetResourceLimitCurrentValues(&cur_commit, reslimit, &type, 1);
		}
		svcCloseHandle(reslimit);
		// on any failure leave both sizes 0: the real allocateHeaps then applies libctru's own split
		if (R_SUCCEEDED(rc) && max_commit > cur_commit) {
			heap_split::split((u32)(max_commit - cur_commit) & ~0xFFFu, heap_split::FORK_MAX_HEAP,
			                  heap_split::FORK_MAX_LINEAR, &__ctru_heap_size, &__ctru_linear_heap_size);
		}
	}
	__real___system_allocateHeaps();
}
}
#endif
