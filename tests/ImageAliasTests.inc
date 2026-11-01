// Included in VulkanHarness: a synthetic headless Vulkan fixture, no guest game.
void CheckKeptImageAliases(bool tiled = false) {
  constexpr const char* name = "KeptImageAliases";
  namespace Memory = Libs::LibKernel::Memory;
  constexpr uint64_t base = 0x205800000ull, allocation_size = 0x200000;
  constexpr uint32_t side = 4, words = side * side, bytes = words * 4;
  const char* mode = std::getenv("KYTY_IMAGE_ALIAS_MODE");
  const bool kept = mode && std::strcmp(mode, "1") == 0;
  EnsureRuntimeContext();
  int64_t physical = -1;
  Require(name, "allocate", Memory::KernelAllocateDirectMemory(0,
      Memory::KernelGetDirectMemorySize(), allocation_size, allocation_size, 0, &physical) == 0,
      "allocation failed");
  void* mapped = reinterpret_cast<void*>(base);
  Require(name, "map", Memory::KernelMapDirectMemory(&mapped, allocation_size, 3, 0x10,
      physical, allocation_size) == 0 && mapped == reinterpret_cast<void*>(base), "mapping failed");
  std::memset(mapped, 0, allocation_size);
  CommandScheduler scheduler(Renderer(), m_runtime_context);
  HW::Context registers{};
  HW::UserConfig config{};
  HW::Shader shaders{};
  scheduler.Begin(registers, config, shaders);
  Renderer().InitializeGpu(nullptr);
  {
    GpuResourceManager resources(m_runtime_context, scheduler);
    resources.SetGpu(&Renderer().GetGpu());
    namespace Exception = Common::HostException;
    Require(name, "fault handler", Exception::InstallHandler([](const Exception::ExceptionInfo& info) {
      if (info.type != Exception::ExceptionType::AccessViolation ||
          (info.access_violation_type != Exception::AccessViolationType::Read &&
           info.access_violation_type != Exception::AccessViolationType::Write)) return false;
      return Memory::HandleGpuFault(info.access_violation_type == Exception::AccessViolationType::Write
          ? PageFaultAccess::Write : PageFaultAccess::Read, info.access_violation_vaddr);
    }), "production fault handler unavailable");
    Memory::InstallGpuResources(&resources);
    resources.MapMemory(base, allocation_size);
    auto& cache = resources.GetTextureCache();
    ImageDesc depth{};
    depth.type = BindingType::DepthTarget;
    depth.info.data = {base, bytes};
    depth.info.pixel_format = vk::Format::eD32Sfloat;
    depth.info.guest_format = Prospero::BufferFormat::k32Float;
    depth.info.type = Prospero::ImageType::kColor2D;
    depth.info.extent = {side, side, 1};
    depth.info.resources = {1, 1};
    depth.info.pitch = TileGetTexturePitch(Prospero::BufferFormat::k32Float, side, Prospero::TileMode::kLinear);
    depth.info.bytes_per_block = 4;
    depth.info.samples = 1;
    depth.info.tile_mode = Prospero::TileMode::kLinear;
    depth.info.data.size = depth.info.pitch * side * 4;
    depth.info.mip_layout[0] = {0, depth.info.data.size, depth.info.pitch, side};
    if (tiled) {
      depth.info.pitch = TileGetDepthPitch(side, 4, 0);
      TileSizeAlign size{};
      Require(name, "attachment layout", TileGetRenderTargetSize(side, side, depth.info.pitch, 4, size, 0),
          "cannot calculate 64-KiB attachment layout");
      depth.info.data.size = size.size;
      depth.info.tile_mode = Prospero::TileMode::kDepth;
      depth.info.mip_layout[0] = {0, size.size, depth.info.pitch, side};
    }
    const auto guest_bytes = depth.info.data.size;
    depth.view_info.format = depth.info.pixel_format;
    depth.view_info.type = vk::ImageViewType::e2D;
    depth.view_info.aspect = vk::ImageAspectFlagBits::eDepth;
    depth.view_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
    auto color = depth;
    color.type = BindingType::Storage;
    color.info.pixel_format = color.view_info.format = vk::Format::eR32Sfloat;
    color.view_info.aspect = vk::ImageAspectFlagBits::eColor;
    color.view_info.usage = vk::ImageUsageFlagBits::eStorage;
    if (tiled) {
      color.info.tile_mode = Prospero::TileMode::kRenderTarget;
      color.info.mip_layout[0] = {0, guest_bytes, color.info.pitch,
          static_cast<uint32_t>(guest_bytes / color.info.pitch / 4)};
    }
    auto integer = color;
    integer.info.pixel_format = integer.view_info.format = vk::Format::eR32Uint;
    integer.info.guest_format = Prospero::BufferFormat::k32UInt;
    auto packed = color;
    packed.info.pixel_format = packed.view_info.format = vk::Format::eB10G11R11UfloatPack32;
    packed.info.guest_format = Prospero::BufferFormat::k11_11_10Float;

    Require(name, "eligible surface", TextureCacheTestAccess::AliasSurface(depth.info, color.info),
        "equal D32/color32 surfaces were rejected");
    for (unsigned test = 0; test != 7; ++test) {
      auto unsupported = color.info;
      switch (test) {
      case 0: unsupported.data.size /= 2; break;
      case 1: unsupported.samples = 4; break;
      case 2: unsupported.resources.layers = 2; break;
      case 3: unsupported.resources.levels = 2; break;
      case 4: unsupported.pitch += 4; break;
      case 5: unsupported.mip_layout[0].offset = 4; break;
      case 6: unsupported.bytes_per_block = 2; break;
      }
      Require(name, "excluded surface", !TextureCacheTestAccess::AliasSurface(depth.info, unsupported),
          "partial/MSAA/mip/layer/layout mismatch was accepted");
    }

    const auto clear = [&](ImageId id, bool is_depth, uint32_t value) {
      vk::ClearValue fill{};
      if (is_depth) fill.depthStencil = vk::ClearDepthStencilValue{std::bit_cast<float>(value), 0};
      else if (cache.GetImage(id).info.pixel_format == vk::Format::eR32Uint)
        fill.color.uint32 = std::array<uint32_t, 4>{value, 0, 0, 0};
      else fill.color = vk::ClearColorValue{std::array<float, 4>{std::bit_cast<float>(value), 0, 0, 0}};
      TextureCacheTestAccess::ClearImage(cache, scheduler.Current(), id,
          {is_depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, fill);
    };
    const auto check = [&](ImageId id, bool is_depth, uint32_t expected) {
      auto download = CreateHostBuffer(name, bytes, vk::BufferUsageFlagBits::eTransferDst, {});
      const vk::BufferImageCopy copy{0, 0, 0,
          {is_depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, 0, 0, 1}, {}, {side, side, 1}};
      cache.GetImage(id).Download(std::span{&copy, 1}, download.buffer, 0, bytes);
      vk::MemoryBarrier2 barrier{};
      barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
      barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
      barrier.dstStageMask = vk::PipelineStageFlagBits2::eHost;
      barrier.dstAccessMask = vk::AccessFlagBits2::eHostRead;
      vk::DependencyInfo dependency{};
      dependency.memoryBarrierCount = 1;
      dependency.pMemoryBarriers = &barrier;
      scheduler.Current().Handle().pipelineBarrier2(dependency);
      scheduler.Finish();
      const auto actual = ReadBuffer(name, download, words);
      Require(name, "native bits", std::ranges::all_of(actual,
          [&](uint32_t v) { return v == expected; }), "alias lost the latest writer's bits: expected=" +
          std::to_string(expected) + " first=" + std::to_string(actual.front()));
      DestroyBuffer(&download);
    };

    auto d = depth;
    auto depth_id = cache.FindImage(d, true);
    clear(depth_id, true, std::bit_cast<uint32_t>(0.25f));
    const auto original_depth = depth_id;
    ImageId original_color{}, original_integer{}, original_packed{};
    for (unsigned pass = 0; pass != 8; ++pass) {
      auto c = color;
      const auto color_id = cache.FindImage(c, true);
      (void)cache.FindTexture(color_id, c);
      check(color_id, false, std::bit_cast<uint32_t>(pass ? 0.75f : 0.25f));
      if (pass == 0) original_color = color_id;
      auto p = packed;
      const auto packed_id = cache.FindImage(p, true);
      (void)cache.FindTexture(packed_id, p);
      check(packed_id, false, std::bit_cast<uint32_t>(pass ? 0.75f : 0.25f));
      if (pass == 0) original_packed = packed_id;
      auto i = integer;
      const auto integer_id = cache.FindImage(i, true);
      (void)cache.FindTexture(integer_id, i);
      if (pass == 0) original_integer = integer_id;
      clear(integer_id, false, std::bit_cast<uint32_t>(0.5f));
      d = depth;
      depth_id = cache.FindImage(d, true);
      (void)cache.FindDepthTarget(depth_id, d);
      check(depth_id, true, std::bit_cast<uint32_t>(0.5f));
      clear(depth_id, true, std::bit_cast<uint32_t>(0.75f));
      if (kept) Require(name, "stable native objects",
          depth_id == original_depth && color_id == original_color && integer_id == original_integer &&
          packed_id == original_packed && TextureCacheTestAccess::AliasCreates(cache) == 3 &&
          TextureCacheTestAccess::AliasMembers(cache, base) == 4,
          "alternation recreated an alias or grew its group");
    }
    if (kept) {
      // Direct acquisition of a cached ID must also synchronize (no FindImage).
      (void)cache.FindTexture(original_color, color);
      check(original_color, false, std::bit_cast<uint32_t>(0.75f));
      // Force a GPU buffer write; FillBuffer may choose a CPU fill for a tiny range.
      auto [buffer, offset] = resources.GetBufferCache().ObtainBuffer(base, guest_bytes, true, false);
      buffer->Fill(offset, guest_bytes, std::bit_cast<uint32_t>(0.375f));
      cache.InvalidateMemoryFromGPU(base, guest_bytes, "alias-test");
      (void)cache.FindDepthTarget(original_depth, depth);
      check(original_depth, true, std::bit_cast<uint32_t>(0.375f));
      scheduler.Finish();
      cache.InvalidateMemory(base, guest_bytes);
      resources.GetBufferCache().InvalidateMemory(base, guest_bytes);
      std::vector<uint32_t> cpu(guest_bytes / 4, std::bit_cast<uint32_t>(0.625f));
      Require(name, "CPU backing", Memory::TryWriteBacking(base, cpu.data(), guest_bytes), "CPU write failed");
      (void)cache.FindTexture(original_color, color);
      check(original_color, false, cpu[0]);

      if (!tiled) {
      auto partial = color;
      partial.info.data.size /= 2;
      partial.info.extent.height /= 2;
      partial.info.mip_layout[0] = {0, partial.info.data.size, partial.info.pitch, side / 2};
      (void)cache.FindImage(partial, true);
      Require(name, "partial fallback", TextureCacheTestAccess::AliasMembers(cache, base) == 0,
          "partial overlap retained a whole-surface group");
      }
    } else {
      Require(name, "mode zero", TextureCacheTestAccess::AliasMembers(cache, base) == 0,
          "disabled mode created a group");
    }
    scheduler.Finish();
    resources.SetGpu(nullptr);
    resources.UnmapMemory(base, allocation_size);
    scheduler.Finish();
    Require(name, "unmap cleanup", TextureCacheTestAccess::AliasMembers(cache, base) == 0,
        "unmap retained an alias owner");
    Memory::InstallGpuResources(nullptr);
  }
  Renderer().ShutdownGpu();
  Require(name, "unmap", Memory::KernelMunmap(base, allocation_size) == 0, "unmap failed");
  Require(name, "release", Memory::KernelReleaseDirectMemory(physical, allocation_size) == 0, "release failed");
  std::printf("[gpu]     %-32s mode=%d tiled=%d ok\n", name, kept ? 1 : 0, tiled ? 1 : 0);
}
