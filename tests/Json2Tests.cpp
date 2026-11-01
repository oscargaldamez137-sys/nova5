// Exercise the actual exported SysV entry points, including explicit sret for
// the guest's non-trivial iterator. No guest executable or GPU is needed.
#include "libs/libJson2.cpp"

#include <array>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
std::map<std::string, uint64_t> exports;
#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "Json2Tests:%d: %s\n", __LINE__, #expression); std::abort(); \
} } while (false)

template <typename T> T Export(const char* nid) {
  CHECK(exports.contains(nid));
  return reinterpret_cast<T>(exports.at(nid));
}
}

namespace Loader {
void SymbolDatabase::Add(const SymbolResolve& symbol, uint64_t address, const std::string&) {
  CHECK(symbol.library == "Json2" && symbol.type == SymbolType::Func);
  CHECK(exports.emplace(symbol.name, address).second);
}
}

int main() {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  using namespace Libs::LibJson2;
  Loader::SymbolDatabase symbols;
  InitNet_1_Json2(&symbols);
  const auto begin = Export<decltype(&JsonArrayBegin)>("bcH5EnFE2xY");
  const auto end = Export<decltype(&JsonArrayEnd)>("WXF2ihRF+B8");
  const auto different = Export<decltype(&JsonArrayIteratorNotEqual)>("5AZPp99ogrc");
  const auto dereference = Export<decltype(&JsonArrayIteratorDereference)>("wcgr5mte7T8");
  const auto increment = Export<decltype(&JsonArrayIteratorIncrement)>("w5+VCznos5E");
  const auto destroy = Export<decltype(&JsonArrayIteratorDtor)>("9yLjn46Ypfs");
  const auto integer = Export<decltype(&JsonValueGetInteger)>("DIxvoy7Ngvk");
  const auto clear = Export<decltype(&JsonValueClearMethod)>("FIjXN2TkuTs");
  const auto parse = Export<decltype(&JsonParserParse)>("S5JxQnoGF3E");

  JsonValue root {};
  JsonValueCtor(&root);
  // The guest's reader at eboot+0x121f920 obtains n through getInteger,
  // reserves n records, then iterates pts using these exported iterators.
  constexpr char text[] = R"({"n":3,"pts":[{"id":1},{"id":2},{"id":-3}],"empty":[],"one":[42]})";
  CHECK(parse(&root, text, sizeof(text)-1) == 0);
  const auto* n = JsonValueIndexString(&root, "n");
  CHECK(n->type == JsonValueTypeUInteger);
  CHECK(*integer(n) == 3); // Previously returned zero and skipped guest reserve.
  const auto* points = JsonValueGetArray(JsonValueIndexString(&root, "pts"));
  CHECK(JsonArraySize(points) == 3);
  struct GuardedIterator {
    uint64_t before = 0x1234567812345678ull;
    JsonArrayIterator value {};
    uint64_t after = 0x8765432187654321ull;
  } first, last;
  CHECK(begin(&first.value, points) == &first.value);
  CHECK(end(&last.value, points) == &last.value);
  const std::array<int64_t, 3> expected {1, 2, -3};
  size_t count = 0;
  while (different(&first.value, &last.value)) {
    CHECK(count < expected.size());
    const auto* item = dereference(&first.value);
    CHECK(item == JsonValueIndexUInt(JsonValueIndexString(&root, "pts"), count));
    CHECK(*integer(JsonValueIndexString(item, "id")) == expected[count]);
    CHECK(increment(&first.value) == &first.value);
    ++count;
  }
  CHECK(count == expected.size());
  CHECK(count == static_cast<size_t>(*integer(n)));
  destroy(&first.value);
  destroy(&last.value);
  for (const auto* it : {&first, &last}) {
    CHECK(it->before == 0x1234567812345678ull && it->after == 0x8765432187654321ull);
  }
  // Empty arrays, including an empty vector with reserved storage, must not
  // fabricate an element or perform arithmetic on a null data() pointer.
  JsonArray empty {};
  JsonArrayCtor(&empty);
  for (int reserved = 0; reserved < 2; ++reserved) {
    if (reserved) empty.impl->reserve(8);
    begin(&first.value, &empty);
    end(&last.value, &empty);
    CHECK(!different(&first.value, &last.value));
    destroy(&first.value); destroy(&last.value);
  }
  JsonArrayDtor(&empty);
  const auto* one = JsonValueGetArray(JsonValueIndexString(&root, "one"));
  begin(&first.value, one); end(&last.value, one);
  CHECK(different(&first.value, &last.value));
  CHECK(*integer(dereference(&first.value)) == 42);
  increment(&first.value);
  CHECK(!different(&first.value, &last.value));
  destroy(&first.value); destroy(&last.value);

  auto* child = const_cast<JsonValue*>(JsonValueIndexString(&root, "pts"));
  auto* parent = child->parent;
  int context = 0;
  child->rootparam = &context;
  clear(child);
  CHECK(child->type == JsonValueTypeNull && child->parent == parent && child->rootparam == &context);
  clear(child); // Idempotent; owning parent must still destroy safely.
  CHECK(*integer(child) == 0 && *integer(nullptr) == 0);
  JsonValueDtor(&root);
  std::puts("Json2Tests: exports, SysV/sret iteration, empty/singleton arrays, signed/unsigned integers and clear passed");
  return 0;
}
