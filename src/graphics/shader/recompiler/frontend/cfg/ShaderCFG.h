#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERCFG_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERCFG_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <vector>

namespace Libs::Graphics::ShaderRecompiler::CFG {

enum class BranchCondition {
	Always,
	SccZero,
	SccNonZero,
	VccZero,
	VccNonZero,
	ExecZero,
	ExecNonZero,
	ScalarInstruction,
	GotoVariable,
	Unknown
};

enum class TerminatorKind { Branch, ConditionalBranch, IndirectBranch, Return, Unsupported };

enum class FailureKind {
	None,
	InvalidInput,
	UnsupportedInstruction,
	InvalidBranchTarget,
	MissingFallthrough,
	InvalidLabel,
	IrreducibleControlFlow,
	StructuredControlFlow
};

struct Terminator {
	TerminatorKind        kind                   = TerminatorKind::Return;
	BranchCondition       condition              = BranchCondition::Always;
	uint32_t              true_block             = UINT32_MAX;
	uint32_t              false_block            = UINT32_MAX;
	uint32_t              merge_block            = UINT32_MAX;
	uint32_t              continue_block         = UINT32_MAX;
	uint32_t              indirect_pc_sgpr       = UINT32_MAX;
	uint32_t              indirect_selector_code = UINT32_MAX;
	std::vector<uint32_t> indirect_target_pcs;
	std::vector<uint32_t> indirect_targets;
	std::vector<uint32_t> indirect_selector_values;
	std::vector<uint32_t> indirect_selector_targets;
	uint32_t              goto_variable = UINT32_MAX;
	int32_t               goto_value    = -1;
	bool                  loop_header   = false;
};

struct BasicBlock {
	uint32_t              id         = 0;
	uint32_t              start_pc   = 0;
	uint32_t              end_pc     = 0;
	uint32_t              inst_begin = 0;
	uint32_t              inst_end   = 0;
	std::vector<uint32_t> predecessors;
	std::vector<uint32_t> successors;
	Terminator            terminator;
};

// The (post-)dominator tree of the last analysis. A block added since has no dominance; a block no
// root reaches (entry and blocks without predecessors; for post-dominance, blocks without
// successors) counts as dominated by every analysed block.
struct DominatorTree {
	enum Kind : uint8_t { Unknown, Tree, All };
	std::vector<uint8_t>  kind;
	std::vector<uint32_t> parent; // UINT32_MAX: a root
	std::vector<uint32_t> depth;  // dominators, the block included
	std::vector<uint32_t> enter;  // preorder number; its subtree ends at `leave`
	std::vector<uint32_t> leave;
	uint32_t              analysed = 0;

	[[nodiscard]] bool                  Dominates(uint32_t dominator, uint32_t block) const;
	[[nodiscard]] uint32_t              Depth(uint32_t block) const;
	[[nodiscard]] uint32_t              NearestCommon(uint32_t a, uint32_t b) const;
	[[nodiscard]] std::vector<uint32_t> Dominators(uint32_t block) const;
	void                                Remap(const std::vector<uint32_t>& id_map);
};

struct BackEdge {
	uint32_t from    = UINT32_MAX;
	uint32_t to      = UINT32_MAX;
	bool     natural = false;
};

struct NaturalLoop {
	uint32_t              header         = UINT32_MAX;
	uint32_t              latch          = UINT32_MAX;
	uint32_t              merge          = UINT32_MAX;
	uint32_t              continue_block = UINT32_MAX;
	std::vector<uint32_t> body_blocks;
	std::vector<uint32_t> exit_blocks;
};

struct StronglyConnectedComponent {
	std::vector<uint32_t> blocks;
	std::vector<uint32_t> entry_blocks;
	bool                  irreducible = false;
};

struct Graph {
	std::vector<BasicBlock>                 blocks;
	std::vector<BackEdge>                   back_edges;
	std::vector<NaturalLoop>                natural_loops;
	std::vector<StronglyConnectedComponent> components;
	std::vector<uint32_t>                   code_table_load_pcs;
	DominatorTree                           dominators;
	DominatorTree                           post_dominators;
	uint32_t                                entry_block   = UINT32_MAX;
	bool                                    irreducible   = false;
	bool                                    unsupported   = false;
	FailureKind                             failure_kind  = FailureKind::None;
	uint32_t                                failure_block = UINT32_MAX;
	std::string                             unsupported_reason;

	const BasicBlock* FindBlock(uint32_t id) const;
	BasicBlock*       FindBlock(uint32_t id);
	const BasicBlock* FindBlockByPc(uint32_t pc) const;
	BasicBlock*       FindBlockByPc(uint32_t pc);
	bool              Dominates(uint32_t dominator, uint32_t block) const;
	bool              PostDominates(uint32_t post_dominator, uint32_t block) const;
	uint32_t          FindNearestCommonPostDominator(uint32_t block_a, uint32_t block_b) const;
};

Graph       BuildGraph(const Decoder::Program& program);
bool        Structurize(Graph& graph);
std::string BranchConditionToString(BranchCondition condition);
std::string FailureKindToString(FailureKind kind);
std::string GraphToString(const Graph& graph);

} // namespace Libs::Graphics::ShaderRecompiler::CFG

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERCFG_H_ */
