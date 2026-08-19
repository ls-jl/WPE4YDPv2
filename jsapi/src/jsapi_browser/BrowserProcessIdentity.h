#pragma once

#include <string>
#include <sys/types.h>

namespace browser {
namespace process {

struct Identity {
    pid_t pid { -1 };
    pid_t processGroup { -1 };
    unsigned long long startTime { 0 };
    std::string runtimePath;
    std::string workdir;
};

bool captureIdentity(pid_t pid, const std::string& runtimePath,
    const std::string& workdir, Identity& identity);
bool identityMatchesRunningProcess(const Identity& identity);
std::string serializeIdentity(const Identity& identity);
bool parseIdentity(const std::string& value, Identity& identity);

} // namespace process
} // namespace browser
