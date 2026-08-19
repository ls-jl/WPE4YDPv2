#include "BrowserFilesystem.h"

#include <cerrno>
#include <fcntl.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace browser {
namespace filesystem {
namespace {

const char* const kDefaultDisplayMode = "native";

std::string joinPath(const std::string& base, const std::string& name)
{
    if (base.empty())
        return name;
    return base[base.size() - 1] == '/' ? base + name : base + "/" + name;
}

std::string trim(const std::string& value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ensureComponent(const std::string& path, mode_t mode, std::string* error)
{
    struct stat status;
    if (lstat(path.c_str(), &status) == 0) {
        if (S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode))
            return true;
        if (error)
            *error = "path exists but is not a directory: " + path;
        return false;
    }
    if (errno != ENOENT) {
        if (error)
            *error = "lstat failed for " + path + ": " + std::strerror(errno);
        return false;
    }

    if (mkdir(path.c_str(), mode) != 0) {
        if (errno == EEXIST && lstat(path.c_str(), &status) == 0
            && S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode))
            return true;
        if (error)
            *error = "mkdir failed for " + path + ": " + std::strerror(errno);
        return false;
    }
    // mkdir is affected by umask. Only a component created by this call may be
    // tightened; existing framework-owned ancestors must remain untouched.
    if (chmod(path.c_str(), mode) != 0) {
        int savedErrno = errno;
        rmdir(path.c_str());
        if (error)
            *error = "chmod failed for new directory " + path + ": " + std::strerror(savedErrno);
        errno = savedErrno;
        return false;
    }
    return true;
}

} // namespace

bool ensureDirectoryTree(const std::string& path, mode_t mode, std::string* error)
{
    if (path.empty()) {
        if (error)
            *error = "directory path is empty";
        return false;
    }

    std::string current;
    size_t position = 0;
    if (path[0] == '/') {
        current = "/";
        position = 1;
    }
    while (position <= path.size()) {
        size_t slash = path.find('/', position);
        std::string component = path.substr(position,
            slash == std::string::npos ? std::string::npos : slash - position);
        if (!component.empty()) {
            if (component == "." || component == "..") {
                if (error)
                    *error = "relative path component is not allowed: " + path;
                return false;
            }
            if (!current.empty() && current[current.size() - 1] != '/')
                current += '/';
            current += component;
            if (!ensureComponent(current, mode, error))
                return false;
        }
        if (slash == std::string::npos)
            break;
        position = slash + 1;
    }
    return true;
}

std::string normalizeDisplayMode(const std::string& value)
{
    const std::string mode = trim(value);
    if (mode == "native" || mode == "rotate90" || mode == "rotate180"
        || mode == "rotate270")
        return mode;
    return std::string();
}

std::string readDisplayMode(const std::string& workdir)
{
    if (workdir.empty())
        return kDefaultDisplayMode;
    const std::string path = joinPath(workdir, "display-mode");
    struct stat status;
    if (lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)
        || status.st_size <= 0 || status.st_size > 32)
        return kDefaultDisplayMode;

    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string mode = input.good() || input.eof()
        ? normalizeDisplayMode(contents.str()) : std::string();
    return mode.empty() ? kDefaultDisplayMode : mode;
}

bool writeDisplayMode(const std::string& workdir, const std::string& value,
    std::string* error)
{
    const std::string mode = normalizeDisplayMode(value);
    if (mode.empty()) {
        if (error)
            *error = "invalid display mode: " + value;
        errno = EINVAL;
        return false;
    }
    if (!ensureDirectoryTree(workdir, 0700, error))
        return false;

    const std::string path = joinPath(workdir, "display-mode");
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back('\0');
    int fd = mkstemp(temporary.data());
    if (fd < 0) {
        if (error)
            *error = "display mode temp create failed: " + std::string(std::strerror(errno));
        return false;
    }

    const std::string payload = mode + "\n";
    bool success = fchmod(fd, 0600) == 0
        && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
    size_t written = 0;
    while (success && written < payload.size()) {
        const ssize_t count = write(fd, payload.data() + written,
            payload.size() - written);
        if (count > 0)
            written += static_cast<size_t>(count);
        else if (count < 0 && errno == EINTR)
            continue;
        else
            success = false;
    }
    if (success)
        success = fsync(fd) == 0;
    const int closeResult = close(fd);
    if (success)
        success = closeResult == 0;
    if (success)
        success = rename(temporary.data(), path.c_str()) == 0;
    if (success) {
        int directoryFd = open(workdir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directoryFd < 0)
            success = false;
        else {
            success = fsync(directoryFd) == 0;
            if (close(directoryFd) != 0)
                success = false;
        }
    }
    if (!success) {
        const int savedErrno = errno;
        unlink(temporary.data());
        if (error)
            *error = "display mode write failed: " + std::string(std::strerror(savedErrno));
        errno = savedErrno;
    }
    return success;
}

} // namespace filesystem
} // namespace browser
