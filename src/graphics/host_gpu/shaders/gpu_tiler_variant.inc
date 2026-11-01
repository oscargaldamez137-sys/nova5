// One SPIR-V module per element size and direction: CMakeLists.txt compiles each family with TILER_ELEMENT_BYTES and
// TILER_TILE defined, so the modules have no specialization constants (issue #43: an RDNA2 driver crashed compiling
// the first tiler pipeline; the tiler was the only pipeline with specialization constants, a uniform buffer and no
// required subgroup size, and has none of them now).
const uint ELEMENT_BYTES = TILER_ELEMENT_BYTES;
const uint TILE = TILER_TILE;
