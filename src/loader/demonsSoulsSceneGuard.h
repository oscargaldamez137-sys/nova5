#pragma once

namespace Loader {
struct Program;
// Demon's Souls' scene node traversals skip a null node instead of dereferencing it (demonsSoulsSceneGuard.cpp).
namespace DemonsSoulsSceneGuard {
void Install(Program* program);
} // namespace DemonsSoulsSceneGuard
} // namespace Loader
