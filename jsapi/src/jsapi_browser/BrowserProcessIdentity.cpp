#include "BrowserProcessIdentity.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <signal.h>
#include <unistd.h>
#include <vector>

namespace browser {
namespace process {
namespace {

bool readProcessStat(pid_t pid, unsigned long long& startTime, char* processState = nullptr)
{
    std::ifstream input(("/proc/" + std::to_string(static_cast<long>(pid)) + "/stat").c_str());
    std::string line;
    if (!input.good() || !std::getline(input, line))
        return false;
    size_t commandEnd = line.rfind(')');
    if (commandEnd == std::string::npos || commandEnd + 2 >= line.size())
        return false;

    std::istringstream fields(line.substr(commandEnd + 2));
    std::string value;
    // The substring starts at field 3 (state); starttime is field 22.
    for (int field = 3; field <= 22; ++field) {
        if (!(fields >> value))
            return false;
        if (field == 3 && processState)
            *processState = value.empty() ? '\0' : value[0];
        if (field == 22) {
            char* end = nullptr;
            errno = 0;
            unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
            if (errno || !end || *end || !parsed)
                return false;
            startTime = parsed;
        }
    }
    return startTime != 0;
}

bool safeValue(const std::string& value)
{
    return value.find('\n') == std::string::npos && value.find('\r') == std::string::npos;
}

} // namespace

bool captureIdentity(pid_t pid, const std::string& runtimePath,
    const std::string& workdir, Identity& identity)
{
    if (pid <= 1 || !safeValue(runtimePath) || !safeValue(workdir))
        return false;
    unsigned long long startTime = 0;
    char processState = '\0';
    if (!readProcessStat(pid, startTime, &processState)
        || processState == 'Z' || processState == 'X')
        return false;
    pid_t processGroup = getpgid(pid);
    if (processGroup <= 1)
        return false;
    identity.pid = pid;
    identity.processGroup = processGroup;
    identity.startTime = startTime;
    identity.runtimePath = runtimePath;
    identity.workdir = workdir;
    return true;
}

bool identityMatchesRunningProcess(const Identity& identity)
{
    if (identity.pid <= 1 || identity.processGroup <= 1 || !identity.startTime)
        return false;
    if (kill(identity.pid, 0) != 0 && errno != EPERM)
        return false;
    unsigned long long currentStartTime = 0;
    char processState = '\0';
    if (!readProcessStat(identity.pid, currentStartTime, &processState)
        || processState == 'Z' || processState == 'X'
        || currentStartTime != identity.startTime)
        return false;
    return getpgid(identity.pid) == identity.processGroup;
}

std::string serializeIdentity(const Identity& identity)
{
    if (identity.pid <= 1 || identity.processGroup <= 1 || !identity.startTime ||
        !safeValue(identity.runtimePath) || !safeValue(identity.workdir))
        return "";
    std::ostringstream output;
    output << "version=1\n";
    output << "pid=" << static_cast<long>(identity.pid) << "\n";
    output << "pgid=" << static_cast<long>(identity.processGroup) << "\n";
    output << "starttime=" << identity.startTime << "\n";
    output << "runtime=" << identity.runtimePath << "\n";
    output << "workdir=" << identity.workdir << "\n";
    return output.str();
}

bool parseIdentity(const std::string& value, Identity& identity)
{
    Identity parsed;
    bool versionValid = false;
    std::istringstream input(value);
    std::string line;
    while (std::getline(input, line)) {
        size_t separator = line.find('=');
        if (separator == std::string::npos)
            return false;
        std::string key = line.substr(0, separator);
        std::string field = line.substr(separator + 1);
        char* end = nullptr;
        errno = 0;
        if (key == "version")
            versionValid = field == "1";
        else if (key == "pid") {
            long number = std::strtol(field.c_str(), &end, 10);
            if (errno || !end || *end || number <= 1)
                return false;
            parsed.pid = static_cast<pid_t>(number);
        } else if (key == "pgid") {
            long number = std::strtol(field.c_str(), &end, 10);
            if (errno || !end || *end || number <= 1)
                return false;
            parsed.processGroup = static_cast<pid_t>(number);
        } else if (key == "starttime") {
            unsigned long long number = std::strtoull(field.c_str(), &end, 10);
            if (errno || !end || *end || !number)
                return false;
            parsed.startTime = number;
        } else if (key == "runtime")
            parsed.runtimePath = field;
        else if (key == "workdir")
            parsed.workdir = field;
    }
    if (!versionValid || parsed.pid <= 1 || parsed.processGroup <= 1 ||
        !parsed.startTime || parsed.runtimePath.empty() || parsed.workdir.empty())
        return false;
    identity = parsed;
    return true;
}

} // namespace process
} // namespace browser
