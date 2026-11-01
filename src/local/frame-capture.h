#ifndef EMULATOR_SRC_LOCAL_FRAME_CAPTURE_H_
#define EMULATOR_SRC_LOCAL_FRAME_CAPTURE_H_
// One-shot record of every guest draw and dispatch, for mapping a title's render
// passes. The live command `capture <dir> [frames]` arms it; recording starts at the
// next guest flip and covers whole frames, one JSON line per call, one file per
// frame (<dir>/frame-N.jsonl). A line carries the call's shaders (hash and code
// address), counts or workgroups, bound targets, textures and compute buffers with
// their guest address, format and extent, key fixed-function state, and the render
// thread's wall time for the call. Formats are raw VkFormat / guest BufferFormat
// numbers. `capture <dir> <frames> <hash,hash,...>` also stores, for calls running one of
// those shaders, user data, the flattened SRT and bound buffers up to 16 KiB as hex words
// (constants: jitter, matrices). Rendering is unchanged, except that the batched draw
// paths (native XPR, indirect draw runs) step aside while a frame is recorded so every
// draw is seen.
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace FrameCapture {

enum : uint8_t { ColorTarget = 0, DepthTarget = 1, Texture = 2 };
enum : uint8_t { StageVs = 0, StagePs = 1, StageCs = 2 };

struct Image {
	uint64_t address = 0, size = 0;
	uint32_t format = 0, guest_format = 0, type = 0, width = 0, height = 0, depth = 0;
	uint32_t levels = 0, layers = 0, pitch = 0, tile = 0, samples = 0, slot = 0;
	uint8_t  kind = Texture, stage = StagePs, storage = 0, written = 0;
};

struct Buffer {
	uint64_t address = 0, size = 0;
	uint8_t  stage = StageCs, written = 0;
};

struct Blob {
	std::string           name;
	uint64_t              address = 0;
	std::vector<uint32_t> words;
};

struct Call {
	const char* name     = nullptr;
	const char* consumed = nullptr; // handled without running the shader (clear, copy)
	bool        dispatch = false;
	bool        prepared = false;   // reached binding preparation
	uint64_t    vs = 0, ps = 0, cs = 0, vs_address = 0, ps_address = 0, cs_address = 0;
	uint32_t    count = 0, instances = 0, groups[3] {}, local[3] {};
	uint64_t    indirect = 0;     // a dispatch's arguments, or the arguments a draw's GPU reads
	uint64_t    index    = 0;     // a draw's index buffer
	uint32_t    index_size = 0;   // INDEX_TYPE (0: 16-bit, 1: 32-bit)
	int32_t     base_vertex = 0;
	float       viewport[6] {}; // xscale xoffset yscale yoffset zscale zoffset
	int32_t     scissor[4] {};  // screen scissor left top right bottom
	uint32_t    target_mask = 0, blend_mask = 0, blend0 = 0; // blend0: src | dst<<8 | op<<16
	uint8_t     z_enable = 0, z_write = 0, z_func = 0;
	std::vector<Image>  images;
	std::vector<Buffer> buffers;
	std::vector<Blob>   blobs;
};

inline std::atomic<int>  g_pending {0}; // frames still to record, set by Arm()
inline std::vector<uint64_t> g_data_hashes; // set by Arm() before recording starts
inline std::vector<uint32_t> g_data_counts; // calls stored per hash in the current frame
constexpr uint32_t           DataCallsPerFrame = 4;
inline std::atomic<bool> g_active {false};
inline std::atomic<int>  g_done {0};     // frames finished by the current request
inline std::mutex        g_mutex;        // guards g_dir (live thread -> Kyty.Gpu)
inline std::string       g_dir;
inline FILE*             g_file  = nullptr; // Kyty.Gpu only from here on
inline uint32_t          g_frame = 0, g_seq = 0;
inline thread_local Call g_call;
// pixdump shares capture's one-frame gate. Completion includes deferred readbacks
// and disk writes; a timed-out request cannot be replaced while they remain live.
inline bool g_pixels = false;
inline std::atomic<uint32_t> g_pixel_pending {0}, g_pixel_saved {0}, g_pixel_failed {0}, g_pixel_skipped {0};
inline uint64_t g_pixel_bytes = 0;
inline uint32_t g_pixel_serial = 0;

[[nodiscard]] inline bool Active() { return g_active.load(std::memory_order_relaxed); }

[[nodiscard]] inline bool PixelSelected() {
	if (!Active() || !g_pixels) return false;
	for (const auto hash: g_data_hashes)
		if (hash != 0 && (hash == g_call.vs || hash == g_call.ps || hash == g_call.cs)) return true;
	return false;
}

// True for the first DataCallsPerFrame calls of a listed shader in each frame.
[[nodiscard]] inline bool WantsData(uint64_t hash) {
	const auto it = std::find(g_data_hashes.begin(), g_data_hashes.end(), hash);
	if (hash == 0 || it == g_data_hashes.end()) return false;
	auto& count = g_data_counts[static_cast<size_t>(it - g_data_hashes.begin())];
	return count < DataCallsPerFrame && ++count != 0;
}

inline void AddWords(std::string name, uint64_t address, const std::vector<uint32_t>& words) {
	g_call.blobs.push_back({std::move(name), address, words});
}

// Guest memory as the renderer uploads it (the backing copy, no faulting read).
inline void AddGuest(std::string name, uint64_t address, uint64_t size) {
	if (address == 0 || size == 0) return;
	size = std::min<uint64_t>(size, 16 * 1024);
	std::vector<uint32_t> words((size + 3) / 4, 0);
	if (!Libs::LibKernel::Memory::TryReadBackingToHost(address, words.data(), size)) return;
	g_call.blobs.push_back({std::move(name), address, std::move(words)});
}

inline void OpenFrame() {
	const std::string path = g_dir + "/frame-" + std::to_string(g_frame) + ".jsonl";
	g_file = std::fopen(path.c_str(), "wb");
	g_seq  = 0;
	std::fill(g_data_counts.begin(), g_data_counts.end(), 0u);
	if (g_file != nullptr) std::fprintf(g_file, "{\"t\":\"frame\",\"frame\":%u}\n", g_frame);
}

// Kyty.Gpu, at every guest flip.
inline void OnFlip() {
	if (g_active.load(std::memory_order_relaxed)) {
		if (g_file != nullptr) {
			std::fprintf(g_file, "{\"t\":\"flip\",\"frame\":%u,\"calls\":%u}\n", g_frame, g_seq);
			std::fclose(g_file);
			g_file = nullptr;
		}
		++g_frame;
		g_done.fetch_add(1, std::memory_order_release);
		if (g_pending.fetch_sub(1, std::memory_order_acq_rel) <= 1) {
			g_active.store(false, std::memory_order_relaxed);
			return;
		}
		OpenFrame();
		return;
	}
	if (g_pending.load(std::memory_order_acquire) <= 0) return;
	{
		const std::lock_guard lock(g_mutex);
		std::error_code       error;
		std::filesystem::create_directories(g_dir, error);
	}
	g_frame = 0;
	OpenFrame();
	g_active.store(true, std::memory_order_relaxed);
}

// Live thread: record the next `frames` frames into `dir`, wait for them. `hashes` is a
// comma-separated list of shader hashes whose constants are stored too.
[[nodiscard]] inline bool Arm(const std::string& dir, int frames, const std::string& hashes,
                              double timeout_seconds, bool pixels = false) {
	if (g_active.load() || g_pending.load() > 0 || g_pixel_pending.load() != 0) return false;
	{
		const std::lock_guard lock(g_mutex);
		g_dir = dir;
	}
	g_data_hashes.clear();
	for (size_t begin = 0; begin < hashes.size();) {
		const auto end = std::min(hashes.find(',', begin), hashes.size());
		if (end > begin) g_data_hashes.push_back(std::strtoull(hashes.substr(begin, end - begin).c_str(), nullptr, 16));
		begin = end + 1;
	}
	g_data_counts.assign(g_data_hashes.size(), 0u);
	if (pixels && g_data_hashes.empty()) return false;
	g_pixels = pixels;
	g_pixel_bytes = 0;
	g_pixel_serial = 0;
	g_pixel_saved.store(0);
	g_pixel_failed.store(0);
	g_pixel_skipped.store(0);
	if (pixels) frames = 1;
	g_done.store(0);
	g_pending.store(frames, std::memory_order_release);
	const auto deadline = std::chrono::steady_clock::now() +
	                      std::chrono::duration<double>(timeout_seconds);
	while (g_done.load(std::memory_order_acquire) < frames || g_pixel_pending.load(std::memory_order_acquire) != 0) {
		if (std::chrono::steady_clock::now() > deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return !pixels || g_pixel_failed.load() == 0;
}

// Renderer ImageInfo (template: this header stays free of renderer types).
template <typename Info>
[[nodiscard]] Image FromInfo(const Info& info, uint8_t kind, uint8_t stage, uint32_t slot) {
	Image m;
	m.address      = info.data.address;
	m.size         = info.data.size;
	m.format       = static_cast<uint32_t>(info.pixel_format);
	m.guest_format = static_cast<uint32_t>(info.guest_format);
	m.type         = static_cast<uint32_t>(info.type);
	m.width        = info.extent.width;
	m.height       = info.extent.height;
	m.depth        = info.extent.depth;
	m.levels       = info.resources.levels;
	m.layers       = info.resources.layers;
	m.pitch        = info.pitch;
	m.tile         = static_cast<uint32_t>(info.tile_mode);
	m.samples      = info.samples;
	m.kind         = kind;
	m.stage        = stage;
	m.slot         = slot;
	return m;
}

inline void Append(std::string& out, const char* format, auto... args) {
	char text[512];
	const int n = std::snprintf(text, sizeof(text), format, args...);
	if (n > 0) out.append(text, static_cast<size_t>(n < static_cast<int>(sizeof(text)) ? n : sizeof(text) - 1));
}

inline void Write(const Call& call, double microseconds) {
	if (g_file == nullptr) return;
	std::string line;
	line.reserve(512 + call.images.size() * 160 + call.buffers.size() * 64);
	Append(line, "{\"i\":%u,\"t\":\"%s\",\"us\":%.2f,\"prep\":%d", g_seq++, call.name ? call.name : "?",
	       microseconds, call.prepared ? 1 : 0);
	if (call.consumed) Append(line, ",\"consumed\":\"%s\"", call.consumed);
	if (call.dispatch) {
		Append(line, ",\"cs\":\"%016" PRIx64 "\",\"cs_va\":\"%" PRIx64 "\",\"groups\":[%u,%u,%u],\"local\":[%u,%u,%u]",
		       call.cs, call.cs_address, call.groups[0], call.groups[1], call.groups[2], call.local[0],
		       call.local[1], call.local[2]);
		if (call.indirect != 0) Append(line, ",\"indirect\":\"%" PRIx64 "\"", call.indirect);
	} else {
		Append(line, ",\"vs\":\"%016" PRIx64 "\",\"ps\":\"%016" PRIx64 "\",\"vs_va\":\"%" PRIx64 "\",\"ps_va\":\"%" PRIx64
		       "\",\"n\":%u,\"inst\":%u",
		       call.vs, call.ps, call.vs_address, call.ps_address, call.count, call.instances);
		if (call.index != 0)
			Append(line, ",\"ib\":\"%" PRIx64 "\",\"it\":%u,\"bv\":%d", call.index, call.index_size, call.base_vertex);
		if (call.indirect != 0) Append(line, ",\"indirect\":\"%" PRIx64 "\"", call.indirect);
		Append(line, ",\"vp\":[%g,%g,%g,%g,%g,%g],\"sc\":[%d,%d,%d,%d]", call.viewport[0], call.viewport[1],
		       call.viewport[2], call.viewport[3], call.viewport[4], call.viewport[5], call.scissor[0],
		       call.scissor[1], call.scissor[2], call.scissor[3]);
		Append(line, ",\"mask\":\"%x\",\"blend\":\"%x\",\"blend0\":\"%x\",\"z\":[%u,%u,%u]", call.target_mask,
		       call.blend_mask, call.blend0, call.z_enable, call.z_write, call.z_func);
	}
	line += ",\"img\":[";
	for (size_t i = 0; i < call.images.size(); ++i) {
		const auto& m = call.images[i];
		Append(line,
		       "%s{\"k\":%u,\"st\":%u,\"slot\":%u,\"a\":\"%" PRIx64 "\",\"sz\":%" PRIu64
		       ",\"f\":%u,\"gf\":%u,\"ty\":%u,\"w\":%u,\"h\":%u,\"d\":%u,\"mip\":%u,\"arr\":%u,\"pitch\":%u,"
		       "\"tile\":%u,\"ms\":%u,\"rw\":%u,\"uav\":%u}",
		       i == 0 ? "" : ",", m.kind, m.stage, m.slot, m.address, m.size, m.format, m.guest_format, m.type,
		       m.width, m.height, m.depth, m.levels, m.layers, m.pitch, m.tile, m.samples, m.written, m.storage);
	}
	line += "],\"buf\":[";
	for (size_t i = 0; i < call.buffers.size(); ++i) {
		const auto& b = call.buffers[i];
		Append(line, "%s{\"st\":%u,\"a\":\"%" PRIx64 "\",\"sz\":%" PRIu64 ",\"rw\":%u}", i == 0 ? "" : ",", b.stage,
		       b.address, b.size, b.written);
	}
	line += "]";
	if (!call.blobs.empty()) {
		line += ",\"data\":[";
		for (size_t i = 0; i < call.blobs.size(); ++i) {
			const auto& blob = call.blobs[i];
			Append(line, "%s{\"n\":\"%s\",\"a\":\"%" PRIx64 "\",\"w\":\"", i == 0 ? "" : ",", blob.name.c_str(),
			       blob.address);
			for (const auto word: blob.words) Append(line, "%08x", word);
			line += "\"}";
		}
		line += "]";
	}
	line += "}\n";
	std::fwrite(line.data(), 1, line.size(), g_file);
}

// Brackets one guest draw or dispatch on Kyty.Gpu; the renderer fills g_call.
class Scope {
public:
	Scope(const char* name, bool dispatch): m_on(Active()) {
		if (!m_on) return;
		g_call.name     = name;
		g_call.consumed = nullptr;
		g_call.dispatch = dispatch;
		g_call.prepared = false;
		g_call.vs = g_call.ps = g_call.cs = 0;
		g_call.vs_address = g_call.ps_address = g_call.cs_address = 0;
		g_call.count = g_call.instances = 0;
		g_call.indirect                 = 0;
		g_call.index = 0, g_call.index_size = 0, g_call.base_vertex = 0;
		for (auto& value: g_call.groups) value = 0;
		for (auto& value: g_call.local) value = 0;
		g_call.images.clear();
		g_call.buffers.clear();
		g_call.blobs.clear();
		m_start = std::chrono::steady_clock::now();
	}
	~Scope() {
		if (!m_on) return;
		const auto elapsed = std::chrono::steady_clock::now() - m_start;
		Write(g_call, std::chrono::duration<double, std::micro>(elapsed).count());
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	bool                                  m_on;
	std::chrono::steady_clock::time_point m_start {};
};

} // namespace FrameCapture

#endif // EMULATOR_SRC_LOCAL_FRAME_CAPTURE_H_
