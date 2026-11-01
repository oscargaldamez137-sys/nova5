#include "graphics/host_gpu/renderer/pipeline/pipelineCacheBlobs.h"

#include "common/stringUtils.h"
#include "local-platform.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string_view>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

constexpr char     Magic[8]   = {'K', 'y', 't', 'y', 'P', 'C', 'B', '1'};
constexpr uint64_t FooterSize = 2 * sizeof(uint64_t) + sizeof(Magic);
constexpr size_t   EntrySize  = sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint64_t);

template <typename T>
void Put(std::string& out, const T& value) {
	out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

// NVIDIA's compression dictionary (a zstd dictionary: its magic number) in a pipeline cache's data, which then holds
// the 2 MiB dictionary itself (a cache of one pipeline is otherwise tens of KiB).
bool HoldsCompressionDictionary(const std::vector<uint8_t>& data) {
	static constexpr uint8_t DictionaryMagic[] = {0x37, 0xa4, 0x30, 0xec};
	return data.size() >= (1u << 20u) &&
	       std::search(data.begin(), data.end(), std::begin(DictionaryMagic), std::end(DictionaryMagic)) != data.end();
}

} // namespace

std::unique_ptr<PipelineCacheBlobs> PipelineCacheBlobs::Open(const std::filesystem::path& path,
                                                             const std::string&           signature) {
	std::error_code error;
	const auto      size = std::filesystem::file_size(path, error);
	if (signature.empty() || error || size < sizeof(Magic) + sizeof(uint32_t) + FooterSize) return nullptr;
	std::unique_ptr<PipelineCacheBlobs> store(new PipelineCacheBlobs);
	store->m_file   = LocalPlatform::OpenFileForReading(Common::PathToString(path).c_str());
	const auto read = [&](uint64_t offset, void* data, size_t bytes) {
		return LocalPlatform::ReadScratchFile(store->m_file, offset, data, bytes);
	};
	char        magic[sizeof(Magic)] {};
	uint32_t    signature_size = 0;
	std::string saved;
	if (store->m_file == 0 || !read(0, magic, sizeof(magic)) || std::memcmp(magic, Magic, sizeof(Magic)) != 0 ||
	    !read(sizeof(Magic), &signature_size, sizeof(signature_size)) || signature_size != signature.size()) {
		return nullptr;
	}
	saved.resize(signature_size);
	const uint64_t data_begin = sizeof(Magic) + sizeof(signature_size) + signature_size;
	uint64_t       footer[2] {};
	if (!read(sizeof(Magic) + sizeof(signature_size), saved.data(), saved.size()) || saved != signature ||
	    !read(size - FooterSize, footer, sizeof(footer)) || !read(size - sizeof(Magic), magic, sizeof(magic)) ||
	    std::memcmp(magic, Magic, sizeof(Magic)) != 0 || footer[0] < data_begin || footer[0] > size - FooterSize) {
		return nullptr;
	}
	std::vector<uint8_t> table(size - FooterSize - footer[0]);
	if (!read(footer[0], table.data(), table.size()) || XXH3_64bits(table.data(), table.size()) != footer[1] ||
	    table.size() < sizeof(uint64_t)) {
		return nullptr;
	}
	uint64_t count = 0;
	std::memcpy(&count, table.data(), sizeof(count));
	if (count > (table.size() - sizeof(count)) / EntrySize) return nullptr;
	store->m_blobs.reserve(count);
	for (uint64_t i = 0; i < count; i++) {
		const auto* at  = table.data() + sizeof(count) + i * EntrySize;
		uint64_t    key = 0;
		Blob        blob;
		std::memcpy(&key, at, sizeof(key));
		std::memcpy(&blob.offset, at + 8, sizeof(blob.offset));
		std::memcpy(&blob.size, at + 16, sizeof(blob.size));
		std::memcpy(&blob.hash, at + 20, sizeof(blob.hash));
		if (blob.offset < data_begin || blob.offset > footer[0] || blob.size > footer[0] - blob.offset) return nullptr;
		store->m_blobs.emplace(key, blob);
	}
	return store;
}

PipelineCacheBlobs::~PipelineCacheBlobs() {
	LocalPlatform::CloseScratchFile(m_file);
}

uint64_t PipelineCacheBlobs::DataBytes() const {
	uint64_t bytes = 0;
	for (const auto& [key, blob]: m_blobs) bytes += blob.size;
	return bytes;
}

bool PipelineCacheBlobs::Load(uint64_t key, std::vector<uint8_t>& data) const {
	const auto found = m_blobs.find(key);
	if (found == m_blobs.end()) return false;
	data.resize(found->second.size);
	return LocalPlatform::ReadScratchFile(m_file, found->second.offset, data.data(), data.size()) &&
	       XXH3_64bits(data.data(), data.size()) == found->second.hash;
}

bool PipelineCacheBlobWriter::Add(uint64_t key, std::vector<uint8_t> data) {
	if (data.empty() || HoldsCompressionDictionary(data)) {
		m_left_out.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	std::lock_guard lock(m_mutex);
	if (m_entries.try_emplace(key, Entry {.data = std::move(data)}).second) m_made.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void PipelineCacheBlobWriter::Copy(uint64_t key, const PipelineCacheBlobs& from) {
	if (!from.Holds(key)) return;
	std::lock_guard lock(m_mutex);
	if (m_entries.try_emplace(key, Entry {.from = &from}).second) m_copied.fetch_add(1, std::memory_order_relaxed);
}

void PipelineCacheBlobWriter::CopyAll(const PipelineCacheBlobs& from) {
	std::lock_guard lock(m_mutex);
	for (const auto& [key, blob]: from.m_blobs) {
		if (m_entries.try_emplace(key, Entry {.from = &from}).second) m_copied.fetch_add(1, std::memory_order_relaxed);
	}
}

bool PipelineCacheBlobWriter::Holds(uint64_t key) const {
	std::lock_guard lock(m_mutex);
	return m_entries.contains(key);
}

size_t PipelineCacheBlobWriter::Pipelines() const {
	std::lock_guard lock(m_mutex);
	return m_entries.size();
}

bool PipelineCacheBlobWriter::Save(const std::filesystem::path& path, const std::string& signature) const {
	std::lock_guard lock(m_mutex);
	std::ofstream   file(path, std::ios::binary | std::ios::trunc);
	if (!file) return false;
	const auto signature_size = static_cast<uint32_t>(signature.size());
	file.write(Magic, sizeof(Magic));
	file.write(reinterpret_cast<const char*>(&signature_size), sizeof(signature_size));
	file.write(signature.data(), static_cast<std::streamsize>(signature.size()));
	uint64_t    offset = sizeof(Magic) + sizeof(signature_size) + signature.size();
	std::string table;
	Put(table, static_cast<uint64_t>(0)); // (the count, once known)
	uint64_t             count = 0;
	std::vector<uint8_t> copied;
	for (const auto& [key, entry]: m_entries) {
		const std::vector<uint8_t>* data = &entry.data;
		if (entry.from != nullptr) {
			if (!entry.from->Load(key, copied)) continue; // (damaged in its file: left out)
			data = &copied;
		}
		file.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
		Put(table, key);
		Put(table, offset);
		Put(table, static_cast<uint32_t>(data->size()));
		Put(table, XXH3_64bits(data->data(), data->size()));
		offset += data->size();
		count++;
	}
	std::memcpy(table.data(), &count, sizeof(count));
	const uint64_t footer[2] = {offset, XXH3_64bits(table.data(), table.size())};
	file.write(table.data(), static_cast<std::streamsize>(table.size()));
	file.write(reinterpret_cast<const char*>(footer), sizeof(footer));
	file.write(Magic, sizeof(Magic));
	return static_cast<bool>(file);
}

} // namespace Libs::Graphics
