void CheckGuestClockGpu(VulkanHarness* vulkan) {
  namespace Memory = Libs::LibKernel::Memory;
  constexpr const char* name = "GuestClockGpu";
  constexpr uint64_t base = 0x209000000ull, bytes = 0x200000;
  Require(name, "mode", GuestClock::Enabled(), "run with KYTY_GPU_TIMESTAMPS=1");
  // Async uploads probe the protected guest view with GuardedCopy before reading the backing.
  EnsureGuestMemoryFaultHandler(name);
  int64_t allocation = -1;
  Require(name, "allocate", Memory::KernelAllocateDirectMemory(
      0, Memory::KernelGetDirectMemorySize(), bytes, 0x10000, 0, &allocation) == 0,
      "cannot allocate timestamp storage");
  void* mapped = reinterpret_cast<void*>(base);
  Require(name, "map", Memory::KernelMapDirectMemory(&mapped, bytes, 3, 0x10, allocation, 0x10000) == 0 &&
      mapped == reinterpret_cast<void*>(base), "cannot map timestamp storage");
  std::memset(mapped, 0, bytes);
  {
    RenderContext context(vulkan->RuntimeContext());
    context.InitializeGpu(nullptr);
    auto& resources = context.GetGpuResources();
    auto& scheduler = context.GetCommandScheduler();
    auto& clock = context.GetGuestClock();
    Memory::InstallGpuResources(&resources);
    context.GetGpu().SendCommandSync([&] {
      resources.MapMemory(base, bytes);
      CommandProcessor processor(context, 0);
      processor.BufferInit();
      const auto record = [&](uint64_t address) {
        processor.WriteAtEndOfPipe64(0, 0, 4, 0, 5, 4, reinterpret_cast<void*>(address), 0, 0);
      };
      const auto read = [&](uint64_t address) {
        uint64_t value = 0;
        Require(name, "backing", Memory::TryReadBacking(address, &value, 8), "backing unavailable");
        return value;
      };
      // More submissions than blocks, with no normal completion drain between them.
      for (uint32_t i = 0; i < 128; ++i) {
        record(base + i * 8u);
        scheduler.Flush();
      }
      scheduler.Finish();
      auto counts = GuestClockTestAccess::Snapshot(clock);
      Require(name, "sparse submissions", counts.recorded == 128 && counts.published == 128 &&
          counts.fallback == 0 && counts.failed == 0 && counts.pending == 0,
          "sparse submissions exhausted the blocks or merged callbacks across ticks");
      std::printf("[clock]   SparseSubmissions ok\n");

      // Fill the entire pool in one open submission: pressure must retire GPU queries, not write CPU clocks.
      constexpr uint32_t dense = 65537;
      for (uint32_t i = 0; i < dense; ++i) record(base + i * 8u);
      scheduler.Finish();
      counts = GuestClockTestAccess::Snapshot(clock);
      Require(name, "pool pressure", counts.recorded == dense + 128u && counts.published == dense + 128u &&
          counts.fallback == 0 && counts.failed == 0 && counts.pending == 0 && counts.waits != 0,
          "pool exhaustion lost timestamps or switched their source");
      uint64_t previous = 0;
      for (uint32_t i = 0; i < dense; ++i) {
        const auto value = read(base + i * 8u);
        Require(name, "query values", value != 0 && value >= previous, "missing or non-monotonic GPU timestamp");
        previous = value;
      }
      std::printf("[clock]   PoolPressure ok\n");

#ifdef KYTY_LOCAL_NATIVE_RESOURCES
      const auto old_frame_mode = kyty_local_frame_pipeline_mode.exchange(1);
      auto& cache = context.GetBufferCache();
      constexpr uint64_t timestamp = base + 0x180000;
      for (const auto writer : {base + 0x100000, timestamp + 0x100, timestamp}) {
        const char* scenario = writer == timestamp ? "overlap"
            : writer == base + 0x100000 ? "other-page" : "same-page";
        std::printf("[clock]   Readback[%s] prepare\n", scenario);
        (void)cache.FindBuffer(base, bytes);
        auto [buffer, offset] = cache.ObtainBuffer(writer, 8, true);
        buffer->Fill(offset, 8, 0x12345678);
        std::printf("[clock]   Readback[%s] prepared\n", scenario);
        const auto request = BufferCacheTestAccess::BeginGuestReadback(cache, writer, 8);
        Require(name, "pending readback", request != nullptr && cache.HasUnpublishedDownloads(),
                "fixture did not create an unpublished readback");
        const bool overlap = writer == timestamp;
        const bool same_page = writer != base + 0x100000;
        Require(name, "overlap", cache.HasPendingBackingPublication(timestamp, 8) == overlap &&
            cache.IsRegionGpuModified(timestamp, 8) == same_page, "unexpected readback/ownership ranges");
        std::printf("[clock]   Readback[%s] pending\n", scenario);
        const auto before = GuestClockTestAccess::Snapshot(clock);
        const auto old_value = read(timestamp);
        record(timestamp);
        scheduler.Finish();
        const auto after = GuestClockTestAccess::Snapshot(clock);
        Require(name, "publication", same_page
            ? after.conflicts == before.conflicts + 1 && read(timestamp) == old_value
            : after.published == before.published + 1 && read(timestamp) != old_value,
            "unrelated readback blocked a clock or a GPU-owned page was overwritten");
        std::printf("[clock]   Readback[%s] clock checked\n", scenario);
#ifdef _WIN32
        // Uploads have finished, but the unpublished readback still owns this protected page.
        uint64_t probe = 0;
        Require(name, "guarded upload probe", Common::GuardedCopy(&probe,
            reinterpret_cast<const void*>(writer), sizeof(probe)) == sizeof(probe),
            "GPU-owned guest bytes were readable or the guarded copy did not stop");
#endif
        BufferCacheTestAccess::CopyGuestReadback(cache, request);
        cache.DrainGuestReadback();
        Require(name, "readback contents", read(writer) == 0x1234567812345678ull,
                "clock publication damaged readback bytes");
        std::printf("[clock]   Readback[%s] ok\n", scenario);
      }
      kyty_local_frame_pipeline_mode.store(old_frame_mode);
      std::printf("[clock]   ReadbackOverlap ok\n");
#endif
      clock.TraceFrame();
      scheduler.Finish();
      resources.UnmapMemory(base, bytes);
    });
    Memory::InstallGpuResources(nullptr);
    context.ShutdownGpu();
  }
  Require(name, "unmap", Memory::KernelMunmap(base, bytes) == 0, "cannot unmap timestamp storage");
  Require(name, "release", Memory::KernelReleaseDirectMemory(allocation, bytes) == 0,
          "cannot release timestamp storage");
}
