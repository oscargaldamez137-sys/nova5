#include "loader/demonsSoulsCopy.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64)
#ifndef XBYAK_NO_EXCEPTION
#define XBYAK_NO_EXCEPTION
#endif
#include <xbyak/xbyak.h>
#endif
namespace {
using namespace Loader::DemonsSoulsCopy;
uint64_t calls = 0, prepared_address = 0, prepared_size = 0;
bool     accept = true;
bool     Prepare(uint64_t address, uint64_t size) {
    ++calls;
    prepared_address = address;
    prepared_size    = size;
    return accept;
}
void Check(bool ok) {
	if (!ok) std::abort();
}
void* KYTY_SYSV_ABI Bridge(void* to, const void* from, size_t size) {
	return Move(to, from, size, Prepare);
}
} // namespace
int main() {
	using namespace Loader::DemonsSoulsCopy;
	for (const size_t size:
	     {size_t {0}, size_t {17}, size_t {262143}, size_t {262144}, size_t {300000}})
		for (const size_t displacement: {size_t {0}, size_t {1}, size_t {31}, size_t {400000}})
			for (const bool reverse: {false, true})
				for (const bool prepare_accepts: {false, true}) {
					std::vector<uint8_t> expected(size + displacement + 64), actual;
					for (size_t i = 0; i < expected.size(); ++i)
						expected[i] = static_cast<uint8_t>((i * 37) ^ (i >> 8));
					actual                 = expected;
					const auto source      = reverse ? displacement + 16 : 16;
					const auto destination = reverse ? 16 : displacement + 16;
					std::memmove(expected.data() + destination, expected.data() + source, size);
					calls  = 0;
					accept = prepare_accepts;
					Check(Bridge(actual.data() + destination, actual.data() + source, size) ==
					      actual.data() + destination);
					Check(actual == expected);
					Check(calls == uint64_t(size >= 262144 && displacement != 0));
					if (calls)
						Check(prepared_address ==
						          reinterpret_cast<uint64_t>(actual.data() + destination) &&
						      prepared_size == size);
				}
#if defined(__x86_64__) || defined(_M_X64)
	Xbyak::CodeGenerator code(128);
	code.push(Xbyak::util::rbp);
	code.mov(Xbyak::util::rbp, Xbyak::util::rsp);
	const auto tail = Tailcall(reinterpret_cast<uint64_t>(&Bridge));
	code.db(tail.data(), tail.size());
	code.ready();
	using Function = void*(KYTY_SYSV_ABI*)(void*, const void*, size_t);
	char output[32] {}, input[32] = "memmove ABI";
	Check(code.getCode<Function>()(output, input, sizeof(input)) == output);
	Check(std::memcmp(output, input, sizeof(input)) == 0);
#endif
	std::puts("DemonsSoulsCopyTests: all cases passed");
}
