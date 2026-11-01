#pragma once

// Astro diagnostic: copy the actual bound GPU buffers before a dangerous draw.
// Publishing the files before that draw necessarily requires completion of the copy.
// Wait(CurrentTick), unlike Finish(), does not retire prepared bindings via callbacks.
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/shader.h"
#include "local/resource-provenance.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace Libs::Graphics::LinkedListCapture {
inline constexpr uint64_t ShaderHash = 0x5457609506174517ull;
inline uint32_t Limit() {
    static const uint32_t count = [] {
        const char* text = std::getenv("KYTY_LINKED_LIST_CAPTURE");
        if (!text || !*text) return 0u;
        char* end = nullptr;
        const auto n = std::strtoul(text, &end, 10);
        if (*end || n > 64 || text[0] == '-') EXIT("KYTY_LINKED_LIST_CAPTURE must be 0..64\n");
        return static_cast<uint32_t>(n);
    }();
    return count;
}
inline std::string Hex(uint64_t value) {
    char text[32]; std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}
inline uint32_t Option(const char* name, uint32_t maximum) {
    const char* text = std::getenv(name);
    if (!text || !*text) return 0;
    char* end = nullptr;
    const auto n = std::strtoul(text, &end, 10);
    if (*end || n > maximum || text[0] == '-') { EXIT("invalid %s\n", name); std::abort(); }
    return static_cast<uint32_t>(n);
}
// Counts matching draws; with per_frame, skip/limit count the first matching draw
// of each frame instead. No GPU work or readback for skipped draws.
struct Selection {
    uint64_t draws = 0, candidates = 0, last_frame = 0;
    bool have_frame = false;
    bool Consider(uint64_t frame, bool per_frame, uint32_t skip) {
        ++draws;
        if (per_frame && have_frame && frame == last_frame) return false;
        have_frame = true; last_frame = frame;
        return ++candidates > skip;
    }
};

inline nlohmann::json Events(uint64_t head, uint64_t head_size, uint64_t node, uint64_t node_size) {
    nlohmann::json events = nlohmann::json::array();
    if (!ResourceProvenance::Enabled()) return events;
    std::lock_guard lock(ResourceProvenance::mutex);
    const auto end = ResourceProvenance::sequence;
    const auto begin = end > ResourceProvenance::history.size() ? end - ResourceProvenance::history.size() : 0;
    for (auto i = begin; i < end; ++i) {
        const auto& e = ResourceProvenance::history[i % ResourceProvenance::history.size()];
        if (!(e.gds ? ResourceProvenance::Overlaps(0xc60, 0x20, e) :
            ResourceProvenance::Overlaps(head, head_size, e) || ResourceProvenance::Overlaps(node, node_size, e))) continue;
        events.push_back({{"sequence", e.sequence}, {"tick", e.tick}, {"timeline", Hex(e.timeline)},
            {"kind", e.kind}, {"hash", Hex(e.hash)}, {"binding", e.binding}, {"gds", e.gds},
            {"address", Hex(e.address)}, {"bytes", e.size}, {"source", Hex(e.source)},
            {"value", e.value}, {"vk_buffer", Hex(e.buffer)}, {"offset", e.offset}, {"range", e.range}});
    }
    return events;
}
inline nlohmann::json producers = nlohmann::json::array(); // GPU command thread; metadata only.
inline uint64_t producer_sequence = 0;
inline void NoteProducer(RenderContext& context, const PreparedBindings& bindings,
                         uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {
    if (!Limit() || !bindings.runtime || !bindings.runtime->program ||
        bindings.runtime->program->shader_hash != 0x6216dac0cc17c2d4ull || bindings.buffer_sources.size() <= 6 ||
        bindings.buffers.size() <= 6) return;
    auto& scheduler = context.GetCommandScheduler();
    const auto& heads = bindings.buffer_sources[6];
    const auto& nodes = bindings.buffer_sources[3];
    nlohmann::json item = {{"sequence", ++producer_sequence}, {"tick", scheduler.CurrentTick()},
        {"timeline", Hex(ResourceProvenance::HandleId(scheduler.GetMasterSemaphore().Handle()))},
        {"frame", context.FrameNumber()}, {"groups", {x, y, z}}, {"indirect_address", Hex(indirect)},
        {"head_address", Hex(heads.address)}, {"head_bytes", heads.size},
        {"node_address", Hex(nodes.address)}, {"node_bytes", nodes.size},
        {"user_data", bindings.runtime->resources.user_data},
        {"events_at_dispatch", Events(heads.address, heads.size, nodes.address, nodes.size)}};
    item["buffers"] = nlohmann::json::array();
    for (const auto i: {6u, 3u}) {
        const auto& native = bindings.buffers[i];
        const auto dword = bindings.runtime->program->bindings.memory_offset_dword + i / 4;
        const auto adjustment = dword < bindings.shader_data.size() ?
            (bindings.shader_data[dword] >> ((i % 4) * 8)) & 255u : 0u;
        item["buffers"].push_back({{"binding", i}, {"vk_buffer", Hex(ResourceProvenance::HandleId(native.buffer))},
            {"offset", native.offset}, {"range", native.range}, {"adjustment", adjustment}});
    }
    if (producers.size() == 16) producers.erase(producers.begin());
    producers.push_back(std::move(item));
}
inline void WriteFile(const std::filesystem::path& path, const void* data, size_t bytes) {
#if defined(_WIN32)
    FILE* file = _wfopen(path.c_str(), L"wb");
#else
    FILE* file = std::fopen(path.c_str(), "wb");
#endif
    if (!file) throw std::runtime_error("cannot create " + path.string());
    bool ok = std::fwrite(data, 1, bytes, file) == bytes;
    ok = std::fflush(file) == 0 && ok;
#if defined(_WIN32)
    ok = _commit(_fileno(file)) == 0 && ok;
#else
    ok = fsync(fileno(file)) == 0 && ok;
#endif
    ok = std::fclose(file) == 0 && ok;
    if (!ok) throw std::runtime_error("cannot persist " + path.string());
}

struct Piece {
    vk::Buffer source;
    uint64_t offset = 0, bytes = 0, staging = 0;
    std::string filename;
};

// Also exercised with synthetic GPU buffers by the host regression harness.
inline void Download(CommandScheduler& scheduler, std::vector<Piece>& pieces,
                     const std::filesystem::path& folder) {
    uint64_t total = 0;
    for (auto& piece: pieces) {
        if (!piece.source || !piece.bytes || ((piece.offset | piece.bytes) & 3) ||
            piece.bytes > (uint64_t{256} << 20) || total > (uint64_t{512} << 20) - piece.bytes)
            throw std::runtime_error("invalid/oversized linked-list capture range");
        piece.staging = total;
        total += piece.bytes;
    }
    if (!total) throw std::runtime_error("empty linked-list capture");
    Buffer staging(scheduler.Graphics(), scheduler, MemoryUsage::Download, 0,
                   vk::BufferUsageFlagBits::eTransferDst, total);
    if (staging.Mapped().size() < total) throw std::runtime_error("capture staging is not mapped");
    auto& command = scheduler.Current();
    command.EndRendering();
    const auto native = command.Handle(); // Flushes any pending compute/copy chain dependency.
    VulkanMemoryBarrier before {};
    before.srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostWrite;
    before.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eTransfer,
                           {}, 1, &before, 0, nullptr, 0, nullptr);
    for (const auto& piece: pieces) {
        const vk::BufferCopy copy {piece.offset, piece.staging, piece.bytes};
        native.copyBuffer(piece.source, staging.Handle(), 1, &copy);
    }
    VulkanMemoryBarrier after {};
    after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    after.dstAccessMask = vk::AccessFlagBits::eHostRead;
    native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                           {}, 1, &after, 0, nullptr, 0, nullptr);
    // Same graphics queue, only this submission, no guest readback or tracker mutation.
    // Do not use Finish(): pending resource destruction must stay at its normal boundary.
    scheduler.Wait(scheduler.CurrentTick());
    staging.Invalidate(0, total);
    for (const auto& piece: pieces)
        WriteFile(folder / piece.filename, staging.Mapped().data() + piece.staging, static_cast<size_t>(piece.bytes));
}

inline bool BeforeDraw(RenderContext& context, const PreparedBindings* bindings) {
    static uint32_t captured = 0; // GPU command thread only.
    static Selection selection;
    if (!Limit() || captured >= Limit() || !bindings || !bindings->runtime ||
        !bindings->runtime->program || bindings->runtime->program->shader_hash != ShaderHash) return false;
    static const bool per_frame = Option("KYTY_LINKED_LIST_CAPTURE_PER_FRAME", 1) != 0;
    static const uint32_t skip = Option("KYTY_LINKED_LIST_CAPTURE_SKIP", 1000000000);
    if (!selection.Consider(context.FrameNumber(), per_frame, skip)) return false;
    try {
        const auto& program = *bindings->runtime->program;
        const auto& resources = bindings->runtime->resources;
        if (program.table_mode || bindings->buffers.size() <= 12 ||
            bindings->buffer_sources.size() != bindings->buffers.size() ||
            resources.buffers.size() != bindings->buffers.size() || program.info.buffers.size() != bindings->buffers.size())
            throw std::runtime_error("capture requires the ordinary resolved descriptor path");
        const char* directory = std::getenv("KYTY_LINKED_LIST_CAPTURE_DIR");
        const std::filesystem::path root = directory && *directory ? directory : "_Build/astro-gpu-debug/linked-list";
        std::filesystem::create_directories(root);
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        auto folder = root / (std::to_string(stamp) + "-" + std::to_string(captured + 1));
        if (!std::filesystem::create_directory(folder)) throw std::runtime_error("capture directory already exists");
        auto& scheduler = context.GetCommandScheduler();
        nlohmann::json meta = {
            {"version", 1}, {"shader_hash", Hex(ShaderHash)}, {"capture", captured + 1},
            {"timeline", Hex(ResourceProvenance::HandleId(scheduler.GetMasterSemaphore().Handle()))},
            {"copy_tick", scheduler.CurrentTick()}, {"phase", "before-draw"},
            {"frame", context.FrameNumber()},
            {"matching_draw", selection.draws}, {"candidate", selection.candidates},
            {"per_frame", per_frame}, {"skip", skip},
            {"synchronous_capture", true}, {"head_binding", 11}, {"node_binding", 12},
            {"node_stride", 8}, {"next_offset", 4}, {"terminator", 0xffffffffu},
            {"user_data", resources.user_data}, {"flattened_srt", resources.flattened_srt},
            {"shader_data", bindings->shader_data},
            {"s33_source", {{"pc", "0x1db0"}, {"binding", 9}, {"byte_offset", 10672}}},
            {"buffers", nlohmann::json::array()}
        };
        meta["producer_history"] = nlohmann::json::array();
        const auto& head = bindings->buffer_sources[11];
        const auto& node = bindings->buffer_sources[12];
        for (const auto& producer: producers) {
            if (producer["head_address"] == Hex(head.address) && producer["head_bytes"] == head.size &&
                producer["node_address"] == Hex(node.address) && producer["node_bytes"] == node.size)
                meta["producer_history"].push_back(producer);
        }
        meta["resource_trace_enabled"] = ResourceProvenance::Enabled();
        meta["events_before_draw"] = Events(head.address, head.size, node.address, node.size);
        meta["history_caveat"] = "Last 16 matching producers and last 1024 resource events; CPU submission metadata, not GPU completion. value is fill value for fill/pm4-dma-fill, first word for pm4-write-data; source is source address/immediate for DMA_DATA.";
        std::vector<Piece> pieces;
        for (size_t i = 0; i < bindings->buffers.size(); ++i) {
            const auto& native = bindings->buffers[i];
            const auto& guest = bindings->buffer_sources[i];
            const auto& raw = resources.buffers[i];
            if (raw.dword_count > raw.dwords.size()) throw std::runtime_error("invalid raw descriptor size");
            const auto dword = program.bindings.memory_offset_dword + i / 4;
            if (dword >= bindings->shader_data.size()) throw std::runtime_error("missing descriptor adjustment");
            const uint64_t adjustment = (bindings->shader_data[dword] >> ((i % 4) * 8)) & 255;
            if (native.range == VK_WHOLE_SIZE || adjustment > native.range || guest.size > native.range - adjustment)
                throw std::runtime_error("invalid resolved descriptor range");
            const auto bytes = (i == 11 || i == 12) ? guest.size : (std::min(guest.size, uint64_t{1} << 20) & ~uint64_t{3});
            if ((i == 11 || i == 12) && !bytes) throw std::runtime_error("empty linked-list descriptor");
            const auto filename = i == 11 ? "heads.bin" : i == 12 ? "nodes.bin" : "buffer-" + std::to_string(i) + ".bin";
            nlohmann::json item = {
                {"binding", i}, {"descriptor_source", program.info.buffers[i].source},
                {"first_pc", Hex(program.info.buffers[i].first_use_pc)},
                {"guest_address", Hex(guest.address)}, {"guest_bytes", guest.size},
                {"vk_buffer", Hex(ResourceProvenance::HandleId(native.buffer))},
                {"descriptor_offset", native.offset}, {"descriptor_range", native.range},
                {"adjustment", adjustment}, {"copy_offset", native.offset + adjustment},
                {"captured_bytes", bytes}, {"truncated", bytes != guest.size},
                {"raw_descriptor", std::vector<uint32_t>(raw.dwords.begin(), raw.dwords.begin() + raw.dword_count)},
                {"file", bytes ? filename : ""}
            };
            meta["buffers"].push_back(std::move(item));
            if (bytes) pieces.push_back({native.buffer, native.offset + adjustment, bytes, 0, filename});
        }
        const auto* gds = context.GetBufferCache().GetGdsBuffer();
        if (!gds || gds->Size() < 0xc80) throw std::runtime_error("GDS capture range unavailable");
        pieces.push_back({gds->Handle(), 0xc60, 0x20, 0, "gds.bin"});
        meta["gds"] = {{"file", "gds.bin"}, {"offset", 0xc60}, {"bytes", 0x20},
                       {"counter_offset", 8}, {"vk_buffer", Hex(ResourceProvenance::HandleId(gds->Handle()))}};
        std::printf("[LINKED_LIST_CAPTURE] copying capture=%u tick=%llu pieces=%zu; waiting before PS draw\n",
            captured + 1, static_cast<unsigned long long>(scheduler.CurrentTick()), pieces.size());
        std::fflush(stdout);
        Download(scheduler, pieces, folder);
        meta["draw_not_submitted"] = true;
        meta["next_draw_tick"] = scheduler.CurrentTick();
        const auto text = meta.dump(2);
        WriteFile(folder / "capture.json.tmp", text.data(), text.size());
        std::filesystem::rename(folder / "capture.json.tmp", folder / "capture.json");
        ++captured;
        std::printf("[LINKED_LIST_CAPTURE] saved %s before draw; capture=%u/%u\n",
                    (folder / "capture.json").string().c_str(), captured, Limit());
        std::fflush(stdout);
        return true;
    } catch (const std::exception& error) {
        EXIT("linked-list capture failed before draw: %s\n", error.what());
        std::abort(); // EXIT is not declared noreturn on every build configuration.
    }
}
} // namespace Libs::Graphics::LinkedListCapture
