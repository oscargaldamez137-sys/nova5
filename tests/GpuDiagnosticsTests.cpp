#include "local/device-fault.h"

#include <cstdio>
#include <cstdlib>

static void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "GpuDiagnosticsTests: %s\n", message); std::exit(1); }
}

static VKAPI_ATTR void VKAPI_CALL FakeBind(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}

int main() {
    using namespace DeviceFault;
    // No Vulkan instance/device: record fake handles through the actual breadcrumb API.
    auto bind = &FakeBind;
    InstallBindHook(&bind);
    const auto command = reinterpret_cast<VkCommandBuffer>(uintptr_t {1});
    const auto pipeline = (VkPipeline)(uintptr_t {2});
    const auto semaphore = (VkSemaphore)(uintptr_t {0x22});
    const uint64_t wait = 17;
    NotePipeline(pipeline, "fixture graphics shader");
    bind(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    NoteSubmit(0x11, 2041, &command, 1, &semaphore, &wait, 1);
    // Reproduce Astro's global ring being overwritten by empty async submissions.
    for (uint64_t tick = 1; tick <= 100; ++tick) NoteSubmit(0x22, tick, nullptr, 0);
    std::string report;
    AppendSubmissions(report);
    Check(report.find("timeline 0x11 tick 2041: 1 binds, 1 pipelines") != std::string::npos,
          "async work evicted the graphics submission");
    Check(report.find("fixture graphics shader") != std::string::npos, "missing pipeline identity");
    Check(report.find("waits for timeline 0x22 >= 17") != std::string::npos, "missing dependency");
    Check(report.find("timeline 0x22 tick 68:") == std::string::npos &&
          report.find("timeline 0x22 tick 69:") != std::string::npos &&
          report.find("timeline 0x22 tick 100:") != std::string::npos, "per-timeline ring boundary");
    // A reused command buffer must not inherit pipelines from its previous submit.
    NoteSubmit(0x11, 2042, &command, 1);
    report.clear();
    AppendSubmissions(report);
    Check(report.find("timeline 0x11 tick 2042: 0 binds, 0 pipelines") != std::string::npos,
          "submission retained stale bindings");
    ReportStall(0x11, 2040, [](uint64_t) { return uint64_t {16}; });
    ReportStall(0x33, 0, [](uint64_t) { return uint64_t {0}; });
    std::puts("GpuDiagnosticsTests: all cases passed (no GPU used)");
}
