#pragma once
#include <cstdint>

// Application heap / linear heap split done by libctru's __system_allocateHeaps() when the app sets no sizes.
// Upstream FourthTube links the bundled libctru fork (library/libctru), whose allocateHeaps.o uses 30 MiB / 50 MiB
// where libctru 2.7.0 uses 24 MiB / 32 MiB (same code, only these two constants differ). On a New 3DS in 124 MB mode
// the 2.7.0 split leaves ~18 MiB less linear memory (mvdstd work buffer, mvd output frame, textures, ndsp).
// Diagnostic builds link libctru 2.7.0 (ABI pairing) and restore the fork's split, see heap_split.cpp.
namespace heap_split {
constexpr uint32_t FORK_MAX_HEAP = 30 << 20;       // fork allocateHeaps.o: 0x1e00000
constexpr uint32_t FORK_MAX_LINEAR = 50 << 20;     // fork allocateHeaps.o: 0x3200000
constexpr uint32_t LIBCTRU_MAX_HEAP = 24 << 20;    // libctru 2.7.0 allocateHeaps.o: 0x1800000
constexpr uint32_t LIBCTRU_MAX_LINEAR = 32 << 20;  // libctru 2.7.0 allocateHeaps.o: 0x2000000

// remaining: free application memory, page aligned (what allocateHeaps computes from the COMMIT resource limit)
inline void split(uint32_t remaining, uint32_t max_heap, uint32_t max_linear, uint32_t *heap, uint32_t *linear) {
	*linear = (remaining / 2) & ~0xFFFu;
	*heap = remaining - *linear;
	if (*heap > max_heap) {
		*heap = max_heap;
		*linear = remaining - max_heap;
		if (*linear > max_linear) {
			*linear = max_linear;
			*heap = remaining - max_linear;
		}
	}
}
} // namespace heap_split
