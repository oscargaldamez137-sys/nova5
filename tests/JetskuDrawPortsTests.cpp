#include "graphics/host_gpu/renderer/pipeline/resourceReuse.h"
#include "graphics/host_gpu/renderer/pipeline/programLookupMemo.h"
#include "graphics/host_gpu/renderer/nativeIndirectPolicy.h"
#include "graphics/host_gpu/renderer/pipeline/shaderReadObserver.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#endif

using namespace Libs::Graphics;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

struct Memory {
	std::array<uint32_t, 32> words {};
	bool clean = true, span = true, fail = false;
	uint64_t end = 0x1080;
	static bool Read(void* p, uint64_t a, uint32_t* out) {
		auto& m = *static_cast<Memory*>(p);
		if (m.fail || a < 0x1000 || a + 4 > m.end) return false;
		*out = m.words[(a - 0x1000) / 4]; return true;
	}
	static bool Span(void* p, uint64_t a, uint32_t* out, uint32_t count, bool) {
		if (!static_cast<Memory*>(p)->span) return false;
		for (uint32_t i = 0; i < count; ++i) if (!Read(p, a + 4*i, out+i)) return false;
		return true;
	}
	static uint64_t End(void* p, uint64_t) { return static_cast<Memory*>(p)->end; }
	static bool Sync(void*, uint64_t, uint64_t) { return true; }
	bool CleanRead(uint64_t a, void* out, size_t n) {
		if (!clean || fail || a < 0x1000 || a + n > end) return false;
		std::memcpy(out, reinterpret_cast<const uint8_t*>(words.data()) + a - 0x1000, n);
		return true;
	}
};

int main() {
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	Memory mem;
	for (uint32_t i = 0; i < mem.words.size(); ++i) mem.words[i] = 0x12340000+i;
	std::array<uint32_t, 3> user {1, 2, 3};
	std::array<uint8_t, PreparedResourceReads::MaxBytes> scratch;
	ResourceReuse::Runtime runtime {.user_data = user, .shader_base = 0x900000000,
	    .read_memory = Memory::Read, .userdata = &mem, .read_specialization_memory = Memory::Read,
	    .sync_memory = Memory::Sync, .try_read_memory_span = Memory::Span, .mapping_end = Memory::End};
	ResourceReuse memo;
	const auto matches = [&] { return memo.Matches(runtime, scratch, [&](auto a, auto p, auto n) { return mem.CleanRead(a, p, n); }); };
	const auto capture = [&] {
		auto r = memo.Begin(runtime, scratch);
		std::array<uint32_t, 4> v;
		bool ok = r.read_memory(r.userdata, 0x1000, v.data());
		ok &= r.read_specialization_memory(r.userdata, 0x1008, v.data());
		ok &= r.try_read_memory_span(r.userdata, 0x1004, v.data(), 4, true);
		(void)r.mapping_end(r.userdata, 0x1000);
		memo.Finish(ok);
	};
	capture(); CHECK(memo.Valid()); CHECK(matches());
	mem.words[31]++; CHECK(matches()); // unobserved bytes do not invalidate
	mem.words[2]++; CHECK(!matches()); // descriptor/branch dependency changed
	capture(); CHECK(matches());
	mem.clean = false; CHECK(!matches()); // GPU dirty, same CPU bytes
	mem.clean = true; capture(); CHECK(matches());
	user[1]++; CHECK(!matches()); user[1]--; CHECK(matches());
	runtime.shader_base++; CHECK(!matches()); runtime.shader_base--;
	mem.end -= 4; CHECK(!matches()); // identical observed bytes, changed mapping
	mem.end += 4; capture(); CHECK(matches());
	// Overlapping observations must agree; final bytes alone do not prove the evaluation.
	auto r = memo.Begin(runtime, scratch); uint32_t word;
	CHECK(r.read_memory(r.userdata, 0x1000, &word)); mem.words[0]++;
	CHECK(r.read_memory(r.userdata, 0x1000, &word)); memo.Finish(true); CHECK(!memo.Valid());
	// A rejected optional span isn't a failed evaluation when scalar fallback succeeds.
	r = memo.Begin(runtime, scratch); mem.span = false;
	CHECK(!r.try_read_memory_span(r.userdata, 0x1000, &word, 1, false));
	CHECK(r.read_memory(r.userdata, 0x1000, &word)); memo.Finish(true); CHECK(matches());
	mem.span = true;
	r = memo.Begin(runtime, scratch); mem.fail = true;
	CHECK(!r.read_memory(r.userdata, 0x1000, &word)); memo.Finish(true); CHECK(!memo.Valid()); mem.fail = false;
	r = memo.Begin(runtime, scratch); CHECK(r.sync_memory(r.userdata, 0x1000, 4)); memo.Finish(true); CHECK(!memo.Valid());
	// Exact snapshot bundle includes uniform-fill and passthrough registers.
	ResourceReuse::Snapshot a, b; CHECK(ResourceReuse::Equal(a,b));
	b.user_data.push_back(1); CHECK(!ResourceReuse::Equal(a,b)); b.user_data.clear();
	b.uniform_fill.value = 7; CHECK(!ResourceReuse::Equal(a,b));
	// Source pointers survive map rehash, never erasure; the generation is checked first.
	ProgramLookupMemo<std::array<uint32_t, 3>, int, int> lookup;
	int source = 1, permutation = 2;
	lookup.Remember(user, &source, 5); lookup.permutation = &permutation;
	CHECK(lookup.Find(user, 5) == &source);
	user[0]++; CHECK(!lookup.Find(user, 5)); user[0]--;
	CHECK(!lookup.Find(user, 6)); CHECK(!lookup.permutation);
	// Jetsku's proof must hold for every possible count, including zero/unbounded.
	for (uint32_t primitive = 1; primitive <= 3; ++primitive)
		for (uint32_t limit = 0; limit < 8; ++limit)
			for (uint32_t count = 0; count < 100; ++count)
				if (NativeIndirect::AlwaysEmpty(true, limit, primitive)) CHECK(std::min(limit, count) < primitive);
	CHECK(!NativeIndirect::AlwaysEmpty(false, 1, 3));
	CHECK(!NativeIndirect::AlwaysEmpty(true, 0, 3));
	CHECK(!NativeIndirect::AlwaysEmpty(true, 3, 3));
	CHECK(!NativeIndirect::AlwaysEmpty(true, 1, 1));
	// Nesting an observer preserves the runtime callback's userdata for mapping probes.
	{ ShaderReadObserver observer([](void*, uint64_t, uint64_t){}, nullptr);
	  ShaderReadObserver::Runtime wrapped(runtime);
	  CHECK(wrapped.Get().mapping_end(wrapped.Get().userdata, 0x1000) == mem.end); }
	CHECK(!ShaderReadObserver::Active());
	std::puts("Jetsku draw ports: host tests passed");
	return 0;
}
