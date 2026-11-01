#pragma once

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <cstring>

namespace Libs::Graphics {

// A short GPU-thread scope for proving that hoisted SRT/descriptor reads do not
// depend on an earlier draw's output. Observation never changes read/sync results.
// A nested materialization inherits the observer; unrelated threads do not.
class ShaderReadObserver {
public:
	using Callback = void (*)(void*, uint64_t, uint64_t);
	ShaderReadObserver(Callback callback, void* userdata)
	    : m_callback(callback), m_userdata(userdata), m_previous(s_current) {
		s_current = this;
	}
	~ShaderReadObserver() { s_current = m_previous; }
	ShaderReadObserver(const ShaderReadObserver&)            = delete;
	ShaderReadObserver& operator=(const ShaderReadObserver&) = delete;
	// Nested observers all see the read, innermost first.
	static void         ObserveRead(uint64_t address, uint64_t size) {
        for (auto* observer = s_current; observer != nullptr; observer = observer->m_previous)
            observer->m_callback(observer->m_userdata, address, size);
	}

	class Runtime {
	public:
		explicit Runtime(const ShaderRecompiler::IR::SrtRuntime& input)
		    : m_original(input), m_runtime(input), m_observer(s_current) {
			if (!m_observer) return;
			m_runtime.userdata    = this;
			m_runtime.read_memory = ReadRaw;
			m_runtime.read_specialization_memory =
			    input.read_specialization_memory ? ReadClean : nullptr;
			m_runtime.sync_memory = input.sync_memory ? Sync : nullptr;
			m_runtime.try_read_memory_span = input.try_read_memory_span ? ReadSpan : nullptr;
		}
		Runtime(const Runtime&)                                                         = delete;
		Runtime&                                              operator=(const Runtime&) = delete;
		[[nodiscard]] const ShaderRecompiler::IR::SrtRuntime& Get() const { return m_runtime; }

	private:
		void Observe(uint64_t address, uint64_t size) const {
			for (auto* observer = m_observer; observer != nullptr; observer = observer->m_previous)
				observer->m_callback(observer->m_userdata, address, size);
		}
		static bool Read(void* userdata, uint64_t address, uint32_t* value, bool clean) {
			auto& self = *static_cast<Runtime*>(userdata);
			self.Observe(address, sizeof(*value));
			const auto reader =
			    clean ? self.m_original.read_specialization_memory : self.m_original.read_memory;
			if (reader) return reader(self.m_original.userdata, address, value);
			std::memcpy(value, reinterpret_cast<const void*>(address), sizeof(*value));
			return true;
		}
		static bool ReadRaw(void* userdata, uint64_t address, uint32_t* value) {
			return Read(userdata, address, value, false);
		}
		static bool ReadSpan(void* userdata, uint64_t address, uint32_t* values, uint32_t count,
		                     bool clean) {
			auto& self = *static_cast<Runtime*>(userdata);
			// Observed spans stay SRT-group sized: a native record keeps every observed word.
			if (count > 16 || !self.m_original.try_read_memory_span(self.m_original.userdata, address,
			                                                        values, count, clean))
				return false;
			self.Observe(address, uint64_t(count) * 4);
			return true;
		}
		static bool ReadClean(void* userdata, uint64_t address, uint32_t* value) {
			return Read(userdata, address, value, true);
		}
		static bool Sync(void* userdata, uint64_t address, uint64_t size) {
			auto& self = *static_cast<Runtime*>(userdata);
			self.Observe(address, size);
			return self.m_original.sync_memory(self.m_original.userdata, address, size);
		}
		ShaderRecompiler::IR::SrtRuntime m_original;
		ShaderRecompiler::IR::SrtRuntime m_runtime;
		ShaderReadObserver*              m_observer;
	};

private:
	inline static thread_local ShaderReadObserver* s_current = nullptr;
	Callback                                       m_callback;
	void*                                          m_userdata;
	ShaderReadObserver*                            m_previous;
};

} // namespace Libs::Graphics
