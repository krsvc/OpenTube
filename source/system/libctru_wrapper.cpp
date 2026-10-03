#include "headers.hpp"

static Mutex resource_lock;

void *linearAlloc_concurrent(size_t size) {
	resource_lock.lock();
	void *res = linearAlloc(size);
	resource_lock.unlock();
	return res;
}
void linearFree_concurrent(void *ptr) {
	resource_lock.lock();
	linearFree(ptr);
	resource_lock.unlock();
}

// libctru's linear heap allocator (MemPool + address rbtree) takes no lock, and most of its callers are libraries that
// bypass the wrappers above: C3D_TexInit (thumbnail thread, via Draw_c2d_image_init), Tex3DS/citro2d (main thread),
// mvdstdInit (decoder initer), linearSpaceFree (menu worker). They ran in parallel with the decoder thread's audio
// buffer alloc/free. The Makefile (-Wl,--wrap=...) routes every call from outside libctru's linear.o through here;
// scripts/check_linear_lock.py verifies that on the linked ELF.
extern "C" {
void *__real_linearAlloc(size_t size);
void *__real_linearMemAlign(size_t size, size_t alignment);
void __real_linearFree(void *mem);
u32 __real_linearSpaceFree(void);

static RecursiveLock linear_pool_lock = {1, 0, 0}; // RecursiveLock_Init() state, usable before static constructors

void *__wrap_linearAlloc(size_t size) {
	RecursiveLock_Lock(&linear_pool_lock);
	void *res = __real_linearAlloc(size);
	RecursiveLock_Unlock(&linear_pool_lock);
	return res;
}
void *__wrap_linearMemAlign(size_t size, size_t alignment) {
	RecursiveLock_Lock(&linear_pool_lock);
	void *res = __real_linearMemAlign(size, alignment);
	RecursiveLock_Unlock(&linear_pool_lock);
	return res;
}
void __wrap_linearFree(void *mem) {
	RecursiveLock_Lock(&linear_pool_lock);
	__real_linearFree(mem);
	RecursiveLock_Unlock(&linear_pool_lock);
}
u32 __wrap_linearSpaceFree(void) {
	RecursiveLock_Lock(&linear_pool_lock);
	u32 res = __real_linearSpaceFree();
	RecursiveLock_Unlock(&linear_pool_lock);
	return res;
}
}

void my_assert(bool condition) {
	if (!condition) {
		volatile int *pointer = NULL;
		*pointer = 100;
	}
}
