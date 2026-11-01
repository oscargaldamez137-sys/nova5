#pragma once

// Executable memory for small generated functions that live as long as the process (linear SRT plans): their code is
// copied into shared chunks instead of each keeping its own Xbyak::CodeGenerator. 20K plans held ~0.85 GB of heap
// (generators, page-aligned growable buffers) and ~0.3 GB of executable pages for a few hundred bytes of code each.
// A chunk is one section mapped twice: written through a read-write view, run from a read-execute view, so a copy
// never changes the protection of pages other threads run. The code must be position independent apart from
// absolute addresses outside it (calls through a register, jumps to its own labels).

#include <cstddef>

namespace JitArena {

// The code's copy (16-byte aligned) for the life of the process; null where the host has no such mapping (the caller
// keeps its own buffer) or when no more memory can be mapped.
const void* Copy(const void* code, size_t size);
// Bytes copied so far (diagnostics).
size_t Used();

} // namespace JitArena
