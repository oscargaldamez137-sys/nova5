// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Compatibility API for the fork's existing callers/tests; one patcher implementation.
#include "loader/guestInstructionPatcher.h"
namespace Loader {
using RedZonePatchResult = GuestInstructionPatchResult;
inline void RegisterRedZonePatchModule(void* p, uint64_t n, void* t, uint64_t tn) {
    RegisterGuestInstructionPatchModule(p, n, t, tn);
}
inline void UnregisterRedZonePatchModule(void* p) { UnregisterGuestInstructionPatchModule(p); }
inline RedZonePatchResult PatchRedZoneMemoryInstructions(uint64_t p, uint64_t n,
                                                         std::span<const uintptr_t> starts) {
    return PatchGuestInstructions(p, n, starts, true, false);
}
}
