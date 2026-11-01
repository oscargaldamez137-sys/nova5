#ifndef EMULATOR_SRC_LIBS_SAVEDATAINFO_H_
#define EMULATOR_SRC_LIBS_SAVEDATAINFO_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace Libs::SaveData {

// System save-list metadata from sce_sys, with legacy fork sidecars as fallback.
// Resolve the requested user/title just like the guest mount and search APIs.
struct SaveSlotInfo {
	std::string title;
	std::string sub_title;
	std::string detail;
	int64_t     mtime = 0;
};

SaveSlotInfo GetSaveSlotInfo(std::string_view dir_name, int32_t user_id, std::string_view title_id = {});

} // namespace Libs::SaveData

#endif /* EMULATOR_SRC_LIBS_SAVEDATAINFO_H_ */
