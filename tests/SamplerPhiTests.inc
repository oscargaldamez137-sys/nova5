// Host-only SPIR-V validation: the sampler's edge choice remains in the GPU
// program even when the branch predicate cannot be evaluated before the draw.
void CheckSamplerPhiSpirv() {
  using namespace ShaderRecompiler::IR;
  namespace CFG = ShaderRecompiler::CFG;
  constexpr const char* name = "SamplerPhiSpirv";
  for (const bool table_mode : {false, true}) {
    for (const uint32_t sample_count : {1u, 3u}) {
    for (uint32_t variant = 0; variant < 5; ++variant) {
      ShaderRecompiler::TranslateResult translated;
      auto& p = translated.program;
      p.stage = ShaderType::Pixel;
      p.wave_size = 32;
      p.srt_plan_complete = true;
      for (uint32_t i = 0; i < 4; ++i) {
        p.block_storage.push_back(std::make_unique<Block>());
        p.blocks.push_back(p.block_storage.back().get());
        p.block_info.push_back({.id = i});
      }
      auto* entry = p.blocks[0]; auto* left = p.blocks[1];
      auto* right = p.blocks[2]; auto* merge = p.blocks[3];
      entry->AddBranch(left); entry->AddBranch(right);
      left->AddBranch(merge); right->AddBranch(merge);
      const auto emit = [](Block* b, ValueOpcode op, std::initializer_list<Value> args = {}) {
        return Value(&b->AppendNewInst(op, args));
      };
      p.block_info[0].condition = emit(entry, ValueOpcode::IEqual32,
          {emit(entry, ValueOpcode::LaneId), Value(0u)});
      emit(entry, ValueOpcode::Reference, {p.block_info[0].condition});
      p.block_info[0].terminator.kind = CFG::TerminatorKind::ConditionalBranch;
      p.block_info[0].terminator.true_block = 1;
      p.block_info[0].terminator.false_block = 2;
      p.block_info[0].terminator.merge_block = 3;
      for (uint32_t i = 1; i < 3; ++i) {
        p.block_info[i].terminator.kind = CFG::TerminatorKind::Branch;
        p.block_info[i].terminator.true_block = 3;
      }
      p.block_info[3].terminator.kind = CFG::TerminatorKind::Return;
      auto& phi = merge->AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
      phi.AddPhiOperand(left, Value(4u)); phi.AddPhiOperand(right, Value(0x104u));
      auto image = emit(entry, ValueOpcode::GetImageResource,
          {Value(0x1000u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u)});
      auto sampler = emit(merge, ValueOpcode::GetSamplerResource,
          {Value(&phi), Value(0u), Value(0u), Value(0u)});
      auto address = emit(merge, ValueOpcode::MakeImageAddress,
          {Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
           Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u)});
      MemoryInfo memory;
      memory.kind = ResourceKind::Image;
      memory.image_dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
      memory.image_address_components = 3;
      memory.dmask = 0xf;
      memory.image_sample_flags = variant == 1 ? ShaderRecompiler::Decoder::ImageSampleFlagLod :
          variant == 2 ? ShaderRecompiler::Decoder::ImageSampleFlagCompare : 0;
      const auto op = variant == 3 ? ValueOpcode::ImageGatherRaw :
                      variant == 4 ? ValueOpcode::ImageQueryLod : ValueOpcode::ImageSampleRaw;
      for (uint32_t sample_index = 0; sample_index < sample_count; ++sample_index) {
        // Before tracking these are guest registers, NOT resource table indices.
        // Zero concealed an orphaned MemoryInfo left behind by Phi splitting.
        memory.resource = 36u + 4u * sample_index;
        memory.sampler = 100u;
        p.memory_info.push_back(memory);
        const auto sample = emit(merge, op, {image, sampler, address});
        sample.Instruction()->SetFlags(MemoryFlags {sample_index, 0x510u + 12u * sample_index});
        for (uint32_t i = 0; i < 4; ++i)
          emit(merge, ValueOpcode::ReferenceU32,
               {emit(merge, ValueOpcode::CompositeExtractU32x4, {sample, Value(i)})});
      }
      TrackResources(p);
      Require(name, "no orphan metadata", p.memory_info.size() == 2 * sample_count,
              "replaced samples left stale metadata");
      for (const auto& tracked : p.memory_info)
        Require(name, "tracked metadata", tracked.resource < p.info.images.size() &&
                tracked.sampler < p.info.samplers.size(), "guest registers survived tracking");
      Require(name, "shared pairs deduplicated", p.info.sampled_pairs.size() == 2, "duplicate pairs");
      Require(name, "two samplers", p.info.samplers.size() == 2, "alternatives were lost");
      ResourceSpecialization specialization;
      specialization.images.resize(1);
      specialization.images[0].numeric_class = Prospero::TextureNumericClass::Float;
      specialization.images[0].dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
      specialization.images[0].mip_count = 1;
      ShaderPixelInputInfo pixel {};
      ShaderRecompiler::CompileOptions options;
      options.stage = ShaderType::Pixel; options.wave_size = 32;
      options.input_info.pixel = &pixel; options.table_mode = table_mode;
      auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization);
      ValidateSpirv(name, compiled.spirv);
      spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
      std::string text;
      Require(name, "disassemble", tools.Disassemble(compiled.spirv, &text), "disassembly failed");
      const char* instruction = variant == 1 ? "OpImageSampleExplicitLod" :
          variant == 2 ? "OpImageSampleDrefImplicitLod" :
          variant == 3 ? "OpImageGather" : variant == 4 ? "OpImageQueryLod" : "OpImageSampleImplicitLod";
      Require(name, "sample alternatives", CountText(text, instruction) == 2 * sample_count, "sampling alternatives not emitted");
      Require(name, "GPU selection", text.find("OpPhi") != std::string::npos &&
              text.find("OpSelect ") != std::string::npos, "GPU edge selection was lost");
      std::printf("SamplerPhiSpirv: samples=%u variant=%u table=%u passed\n", sample_count, variant, table_mode);
    }
    }
  }
}
