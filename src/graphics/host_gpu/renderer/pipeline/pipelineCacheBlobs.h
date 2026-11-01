#pragma once
// Without VK_KHR_pipeline_binary: the static precompile's pipelines, each as the data of a pipeline cache that held it
// alone, in one file (_PipelineCache/static/<title>_<version>.blobs) indexed by the pipeline's identity
// (GraphicsPipelineIdentity, ComputePipelineIdentity). The emulator makes a pipeline's cache of its data when the game
// first needs the pipeline; a static VkPipelineCache of every pipeline the driver instead copies into memory whole when it
// is loaded (4.5 GB for Demon's Souls: an 8 GB-GPU laptop with 32 GB of RAM ran at its commit limit).
//
// File: magic, signature (the GPU and driver), the blobs' data, then the table (key, offset, size, XXH3 of each) and a
// footer pointing at it (its offset and XXH3, magic).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

class PipelineCacheBlobs {
public:
	// The file at `path` when it was made on this GPU and driver (signature), else none.
	static std::unique_ptr<PipelineCacheBlobs> Open(const std::filesystem::path& path, const std::string& signature);
	~PipelineCacheBlobs();

	// A pipeline's cache data (false: none, or damaged).
	bool                   Load(uint64_t key, std::vector<uint8_t>& data) const;
	[[nodiscard]] bool     Holds(uint64_t key) const { return m_blobs.contains(key); }
	[[nodiscard]] size_t   Pipelines() const { return m_blobs.size(); }
	[[nodiscard]] uint64_t DataBytes() const;

private:
	friend class PipelineCacheBlobWriter;
	struct Blob {
		uint64_t offset = 0;
		uint32_t size   = 0;
		uint64_t hash   = 0;
	};
	uint64_t                           m_file = 0;
	std::unordered_map<uint64_t, Blob> m_blobs;
};

// The static precompile's output: each pipeline's cache data, made in the precompile or copied from another file.
// Thread-safe.
class PipelineCacheBlobWriter {
public:
	// A pipeline made in a cache of its own; false (left out) when its data holds NVIDIA's compression dictionary: a
	// process trains one after a few hundred pipelines and puts it into every pipeline cache's data from then on (2 MiB
	// each), so the shard runs again as two (precompile-windows.ps1), as with pipeline binaries.
	bool Add(uint64_t key, std::vector<uint8_t> data);
	// A pipeline another file holds (its data is read when this is saved).
	void Copy(uint64_t key, const PipelineCacheBlobs& from);
	// Every pipeline of another file.
	void CopyAll(const PipelineCacheBlobs& from);
	// Written whole to `path` (the caller renames a temporary file).
	bool                 Save(const std::filesystem::path& path, const std::string& signature) const;
	[[nodiscard]] bool   Holds(uint64_t key) const;
	[[nodiscard]] size_t Pipelines() const;
	[[nodiscard]] size_t Made() const { return m_made.load(std::memory_order_relaxed); }
	[[nodiscard]] size_t Copied() const { return m_copied.load(std::memory_order_relaxed); }
	[[nodiscard]] size_t LeftOut() const { return m_left_out.load(std::memory_order_relaxed); }

private:
	struct Entry {
		std::vector<uint8_t>      data;           // made, or
		const PipelineCacheBlobs* from = nullptr; // copied: that file's blob
	};
	mutable std::mutex                  m_mutex;
	std::unordered_map<uint64_t, Entry> m_entries;
	std::atomic<size_t>                 m_made {0}, m_copied {0}, m_left_out {0};
};

} // namespace Libs::Graphics
