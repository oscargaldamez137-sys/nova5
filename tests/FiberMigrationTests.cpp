#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <semaphore>
#include <thread>

namespace Libs::Fiber {
struct FiberObject;
struct FiberOptParam;
using FiberEntry = KYTY_SYSV_ABI void (*)(uint64_t, uint64_t);
int32_t KYTY_SYSV_ABI FiberInitialize(FiberObject*, const char*, FiberEntry, uint64_t,
                                    void*, uint64_t, const FiberOptParam*, uint32_t);
int32_t KYTY_SYSV_ABI FiberRun(FiberObject*, uint64_t, uint64_t*);
int32_t KYTY_SYSV_ABI FiberReturnToThread(uint64_t, uint64_t*);
int32_t KYTY_SYSV_ABI FiberGetSelf(FiberObject**);
int32_t KYTY_SYSV_ABI FiberFinalize(FiberObject*);
}

namespace {
using namespace Libs::Fiber;
void Check(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FiberMigrationTests: %s\n", message);
    std::abort();
  }
}

void KYTY_SYSV_ABI Entry(uint64_t expected_address, uint64_t argument) {
  auto* expected = reinterpret_cast<FiberObject*>(expected_address);
  for (uint64_t step = 0; step < 4; ++step) {
    FiberObject* current = nullptr;
    Check(FiberGetSelf(&current) == 0 && current == expected,
          "resumed fiber must belong to the current host thread");
    Check(argument == step, "resume argument must survive migration");
    Check(FiberReturnToThread(step + 1, &argument) == 0,
          "return from migrated fiber");
  }
  std::abort();
}
}

int main() {
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::OutputDirection::Silent;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();

  alignas(16) std::array<std::byte, 256> object {};
  alignas(16) std::array<std::byte, 64 * 1024> stack {};
  auto* fiber = reinterpret_cast<FiberObject*>(object.data());
  Check(FiberInitialize(fiber, "migration", Entry, reinterpret_cast<uint64_t>(fiber),
                        stack.data(), stack.size(), nullptr, 0x03500000) == 0,
        "initialize fiber");
  auto run = [&](uint64_t step) {
    uint64_t returned = 0;
    Check(FiberRun(fiber, step, &returned) == 0 && returned == step + 1,
          "run and yield on destination thread");
    FiberObject* current = fiber;
    Check(FiberGetSelf(&current) == 0 && current == nullptr,
          "destination thread must have no active fiber after yield");
  };
  run(0);
  std::binary_semaphore first_done(0), second_done(0);
  std::thread first([&] {
    run(1);
    first_done.release();
    second_done.acquire();
  });
  std::thread second([&] {
    first_done.acquire();
    run(2);
    second_done.release();
  });
  first.join();
  second.join();
  run(3);
  Check(FiberFinalize(fiber) == 0, "finalize suspended fiber");
  subsystems.Destroy();
  std::puts("FiberMigrationTests: all cases passed");
}
