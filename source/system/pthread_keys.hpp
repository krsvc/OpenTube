#pragma once
// Key bookkeeping for the pthread_key_* shim in fake_pthread.cpp (devkitARM r68 / gcc 16 libstdc++
// requires these symbols). Pure C++ so it is host-testable. Per-thread VALUES are not stored here:
// fake_pthread.cpp keeps them in a `__thread` array, which libctru allocates and frees with each
// thread (no slot table, no thread-id reuse, no ENOMEM). This header owns the key table and the
// POSIX destructor pass that a thread runs when it exits.
#include <cstddef>

namespace fake_pthread_keys {

static const int MAX_KEYS = 8;
static const int MAX_DESTRUCTOR_PASSES = 4; // POSIX: PTHREAD_DESTRUCTOR_ITERATIONS

typedef void (*Destructor)(void *);

struct KeyTable {
	bool used[MAX_KEYS];
	Destructor dtor[MAX_KEYS];

	KeyTable() {
		for (int i = 0; i < MAX_KEYS; i++) {
			used[i] = false;
			dtor[i] = NULL;
		}
	}
	// returns the key index or -1 when all keys are in use (caller reports EAGAIN)
	int create(Destructor d) {
		for (int i = 0; i < MAX_KEYS; i++) {
			if (!used[i]) {
				used[i] = true;
				dtor[i] = d;
				return i;
			}
		}
		return -1;
	}
	bool destroy(int key) {
		if (!valid(key)) {
			return false;
		}
		used[key] = false;
		dtor[key] = NULL;
		return true;
	}
	bool valid(int key) const { return key >= 0 && key < MAX_KEYS && used[key]; }
	// Run the destructors of one exiting thread over its value array (POSIX semantics: a
	// destructor may set new values, so repeat up to MAX_DESTRUCTOR_PASSES times).
	// Returns the number of destructor invocations.
	int run_destructors(const void *values[MAX_KEYS]) const {
		int calls = 0;
		for (int pass = 0; pass < MAX_DESTRUCTOR_PASSES; pass++) {
			bool any = false;
			for (int k = 0; k < MAX_KEYS; k++) {
				if (used[k] && values[k] && dtor[k]) {
					void *v = (void *)values[k];
					values[k] = NULL;
					dtor[k](v);
					calls++;
					any = true;
				}
			}
			if (!any) {
				break;
			}
		}
		return calls;
	}
};

} // namespace fake_pthread_keys
