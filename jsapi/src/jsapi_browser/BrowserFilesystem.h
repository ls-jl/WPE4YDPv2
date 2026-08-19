#pragma once

#include <string>
#include <sys/types.h>

namespace browser {
namespace filesystem {

// Creates missing path components without changing permissions on components
// that are already owned by the MiniApp framework.
bool ensureDirectoryTree(const std::string& path, mode_t mode, std::string* error = nullptr);

// Display modes are persisted outside profiles because they describe the
// physical relationship between the MiniApp and the panel, not website data.
std::string normalizeDisplayMode(const std::string& value);
std::string readDisplayMode(const std::string& workdir);
bool writeDisplayMode(const std::string& workdir, const std::string& value,
    std::string* error = nullptr);

} // namespace filesystem
} // namespace browser
