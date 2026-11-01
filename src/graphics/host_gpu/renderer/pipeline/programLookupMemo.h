#pragma once
#include <cstdint>

namespace Libs::Graphics {
// Jetsku c7c4da45: exact last-source/permutation lookup. Nova5 can erase warm
// sources, unlike Jetsku; invalidate the pointers BEFORE testing their contents.
template <typename Key, typename Source, typename Permutation>
struct ProgramLookupMemo {
	Key key;
	Source* source = nullptr;
	const Permutation* permutation = nullptr;
	uint64_t generation = 0;
	Source* Find(const Key& candidate, uint64_t erased) {
		if (generation != erased) { source = nullptr; permutation = nullptr; }
		return source && key == candidate ? source : nullptr;
	}
	void Remember(const Key& candidate, Source* value, uint64_t erased) {
		key = candidate;
		source = value;
		permutation = nullptr;
		generation = erased;
	}
};
}
