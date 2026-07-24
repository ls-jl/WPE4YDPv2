#include "jqutil_v2/jqutil.h"
#include "utils/log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <strings.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef __linux__
#include <sys/prctl.h>
#endif

using namespace JQUTIL_NS;

namespace browser {
namespace {

const char* kTag = "miniapp-browser-launcher";
const char* kRuntimeRelative = "assets/wpe-runtime";
const char* kDefaultUrl = "https://m.baidu.com/";
const char* kDefaultViewport = "960x266";
const int kDefaultRotation = 270;

static std::string joinPath(const std::string& base, const std::string& name)
{
    if (base.empty()) return name;
    if (base[base.size() - 1] == '/') return base + name;
    return base + "/" + name;
}

static bool pathExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool dirExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool fileExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static bool ensureDir(const std::string& path)
{
    if (path.empty()) return false;
    if (mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) {
        chmod(path.c_str(), 0700);
        return true;
    }
    return false;
}

static bool ensureDirRecursive(const std::string& path)
{
    if (path.empty()) return false;
    std::string current;
    size_t pos = 0;
    if (path[0] == '/') {
        current = "/";
        pos = 1;
    }
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!part.empty()) {
            if (!current.empty() && current[current.size() - 1] != '/') current += "/";
            current += part;
            if (!ensureDir(current)) return false;
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

static std::string parentDir(const std::string& path)
{
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

static bool removeRecursive(const std::string& path)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        return unlink(path.c_str()) == 0 || errno == ENOENT;
    }

    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    bool ok = true;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        if (!removeRecursive(joinPath(path, entry->d_name))) ok = false;
    }
    closedir(dir);
    if (rmdir(path.c_str()) != 0 && errno != ENOENT) ok = false;
    return ok;
}

static std::string readFile(const std::string& path)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input.good()) return "";
    std::ostringstream ss;
    ss << input.rdbuf();
    return ss.str();
}

static bool writeFile(const std::string& path, const std::string& value, mode_t mode = 0600, bool syncToDisk = true)
{
    ensureDirRecursive(parentDir(path));
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    const char* ptr = value.data();
    size_t left = value.size();
    bool ok = true;
    while (left > 0) {
        ssize_t written = write(fd, ptr, left);
        if (written <= 0) {
            ok = false;
            break;
        }
        ptr += written;
        left -= static_cast<size_t>(written);
    }
    if (syncToDisk && fsync(fd) != 0) ok = false;
    close(fd);
    chmod(path.c_str(), mode);
    return ok;
}

static bool writeFileAtomic(const std::string& path, const std::string& value,
    mode_t mode = 0600, bool syncToDisk = true)
{
    ensureDirRecursive(parentDir(path));
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    int fd = mkstemp(buffer.data());
    if (fd < 0) return false;
    fchmod(fd, mode);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    const char* ptr = value.data();
    size_t left = value.size();
    bool ok = true;
    while (left > 0) {
        ssize_t written = write(fd, ptr, left);
        if (written <= 0) {
            ok = false;
            break;
        }
        ptr += written;
        left -= static_cast<size_t>(written);
    }
    if (ok && syncToDisk && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(buffer.data(), path.c_str()) != 0) ok = false;
    if (!ok) unlink(buffer.data());
    if (ok) chmod(path.c_str(), mode);
    return ok;
}

static std::string jsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

static JSValue getObjectProperty(JSContext* ctx, JSValueConst obj, const char* name)
{
    if (!JS_IsObject(obj)) return JS_UNDEFINED;
    return JS_GetPropertyStr(ctx, obj, name);
}

static std::string getStringProperty(JSContext* ctx, JSValueConst obj, const char* name, const std::string& fallback)
{
    JSValue value = getObjectProperty(ctx, obj, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return fallback;
    }
    const char* str = JS_ToCString(ctx, value);
    if (!str) {
        JS_FreeValue(ctx, value);
        return fallback;
    }
    std::string result(str);
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, value);
    return result.empty() ? fallback : result;
}

static int getIntProperty(JSContext* ctx, JSValueConst obj, const char* name, int fallback)
{
    JSValue value = getObjectProperty(ctx, obj, name);
    if (!JS_IsNumber(value)) {
        JS_FreeValue(ctx, value);
        return fallback;
    }
    int32_t result = fallback;
    JS_ToInt32(ctx, &result, value);
    JS_FreeValue(ctx, value);
    return result;
}

static bool getBoolProperty(JSContext* ctx, JSValueConst obj, const char* name, bool fallback)
{
    JSValue value = getObjectProperty(ctx, obj, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return fallback;
    }
    int result = JS_ToBool(ctx, value);
    JS_FreeValue(ctx, value);
    return result < 0 ? fallback : result != 0;
}

static bool processExists(pid_t pid)
{
    return pid > 1 && (kill(pid, 0) == 0 || errno == EPERM);
}

static pid_t readPidFile(const std::string& path)
{
    if (path.empty()) return -1;
    FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp) return -1;
    long pid = -1;
    if (std::fscanf(fp, "%ld", &pid) != 1) pid = -1;
    std::fclose(fp);
    return pid > 1 ? static_cast<pid_t>(pid) : -1;
}

static bool writePidFile(const std::string& path, pid_t pid)
{
    return writeFile(path, std::to_string(static_cast<long>(pid)) + "\n", 0600);
}

static bool isSafeKeyboardId(const std::string& value)
{
    if (value.empty() || value.size() > 96) return false;
    for (char ch : value) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '_' || ch == '-') continue;
        return false;
    }
    return true;
}

static std::string keyboardDirForWorkdir(const std::string& workdir)
{
    return joinPath(workdir, "keyboard");
}

static bool prepareKeyboardDirs(const std::string& workdir)
{
    if (workdir.empty()) return false;
    const std::string keyboardDir = keyboardDirForWorkdir(workdir);
    return ensureDirRecursive(joinPath(keyboardDir, "requests")) &&
        ensureDirRecursive(joinPath(keyboardDir, "responses")) &&
        ensureDirRecursive(joinPath(keyboardDir, "status"));
}

static int waitForProcessExit(pid_t pid, int timeoutMs)
{
    if (pid <= 1) return 0;
    int status = 0;
    const int slices = timeoutMs / 100;
    for (int i = 0; i < slices; ++i) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) return 0;
        if (result < 0 && errno == ECHILD) return processExists(pid) ? -1 : 0;
        if (!processExists(pid)) return 0;
        usleep(100000);
    }
    return -1;
}

static pid_t parseProcPid(const char* name)
{
    if (!name || !name[0]) return -1;
    char* end = nullptr;
    errno = 0;
    long value = std::strtol(name, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value <= 1) return -1;
    return static_cast<pid_t>(value);
}

static bool containsString(const std::string& value, const std::string& needle)
{
    return !needle.empty() && value.find(needle) != std::string::npos;
}

static std::string toLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

static std::string trimAscii(const std::string& value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

static bool isBrowserProcessName(const std::string& cmdline)
{
    return cmdline.find("wpe-drm-minimal") != std::string::npos ||
        cmdline.find("WPEWebProcess") != std::string::npos ||
        cmdline.find("WPENetworkProcess") != std::string::npos;
}

static void signalProcessTree(pid_t pid, int signo)
{
    if (pid <= 1) return;
    pid_t pgid = getpgid(pid);
    if (pgid > 1) kill(-pgid, signo);
    kill(pid, signo);
}

static std::vector<pid_t> collectScopedBrowserPids(const std::string& runtimePath, const std::string& workdir)
{
    std::vector<pid_t> pids;
    if (runtimePath.empty() && workdir.empty()) return pids;

    DIR* proc = opendir("/proc");
    if (!proc) return pids;

    struct dirent* entry = nullptr;
    while ((entry = readdir(proc)) != nullptr) {
        pid_t pid = parseProcPid(entry->d_name);
        if (pid <= 1 || pid == getpid()) continue;

        const std::string procDir = joinPath("/proc", entry->d_name);
        std::string cmdline = readFile(joinPath(procDir, "cmdline"));
        if (cmdline.empty() || !isBrowserProcessName(cmdline)) continue;

        std::string environ = readFile(joinPath(procDir, "environ"));
        const bool scoped = (!runtimePath.empty() && (containsString(cmdline, runtimePath) ||
            containsString(environ, runtimePath))) ||
            (!workdir.empty() && (containsString(cmdline, workdir) ||
            containsString(environ, workdir)));
        if (scoped) pids.push_back(pid);
    }
    closedir(proc);
    return pids;
}

static void cleanupScopedBrowserProcesses(const std::string& runtimePath, const std::string& workdir)
{
    std::vector<pid_t> pids = collectScopedBrowserPids(runtimePath, workdir);
    if (pids.empty()) return;
    for (pid_t pid : pids) signalProcessTree(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        bool anyRunning = false;
        for (pid_t pid : pids) {
            if (processExists(pid)) {
                anyRunning = true;
                break;
            }
        }
        if (!anyRunning) return;
        usleep(100000);
    }
    for (pid_t pid : pids) signalProcessTree(pid, SIGKILL);
}

static bool directoryHasFilePrefix(const std::string& path, const std::string& prefix)
{
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    bool found = false;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        if (!std::strncmp(entry->d_name, prefix.c_str(), prefix.size())) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

static bool directoryHasFontFile(const std::string& path)
{
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    bool found = false;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        size_t len = std::strlen(name);
        if (len > 4) {
            const char* ext = name + len - 4;
            if (!strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf") || !strcasecmp(ext, ".ttc")) {
                found = true;
                break;
            }
        }
    }
    closedir(dir);
    return found;
}

static bool chmodIfExists(const std::string& path, mode_t mode)
{
    if (!pathExists(path)) return true;
    return chmod(path.c_str(), mode) == 0;
}

static void cleanupOldExtractedRuntime(const std::string& dataDir)
{
    if (dataDir.empty()) return;
    const std::string browserDir = joinPath(dataDir, "browser");
    removeRecursive(joinPath(browserDir, "runtime"));
    removeRecursive(joinPath(browserDir, "runtime.old"));
    unlink(joinPath(browserDir, ".installed.json").c_str());

    DIR* dir = opendir(browserDir.c_str());
    if (!dir) return;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (!std::strncmp(entry->d_name, "runtime.tmp.", 12))
            removeRecursive(joinPath(browserDir, entry->d_name));
    }
    closedir(dir);
}

static bool ensurePackagedRuntimeReady(const std::string& runtimeDir, std::string& error)
{
    if (!dirExists(runtimeDir)) {
        error = "runtime dir missing: " + runtimeDir;
        return false;
    }

    const std::vector<std::string> requiredFiles = {
        "run.sh",
        "wpe-drm-minimal",
        "etc/ssl/certs/ca-certificates.crt",
        "libexec/wpe-webkit-2.0/WPEWebProcess",
        "libexec/wpe-webkit-2.0/WPENetworkProcess",
    };
    for (const std::string& file : requiredFiles) {
        const std::string path = joinPath(runtimeDir, file);
        if (!fileExists(path)) {
            error = "runtime file missing: " + path;
            return false;
        }
    }

    if (!directoryHasFilePrefix(joinPath(runtimeDir, "lib"), "libWPEWebKit-2.0.so")) {
        error = "runtime WebKit library missing: " + joinPath(runtimeDir, "lib");
        return false;
    }
    if (!directoryHasFontFile(joinPath(runtimeDir, "assets/fonts/dejavu"))) {
        error = "runtime fonts missing: " + joinPath(runtimeDir, "assets/fonts/dejavu");
        return false;
    }

    const std::vector<std::string> executableFiles = {
        "run.sh",
        "wpe-drm-minimal",
        "libexec/wpe-webkit-2.0/WPEWebProcess",
        "libexec/wpe-webkit-2.0/WPENetworkProcess",
        "libexec/wpe-webkit-2.0/WPEGPUProcess",
        "libexec/gstreamer-1.0/gst-plugin-scanner",
        "libexec/gstreamer-1.0/gst-ptp-helper",
    };
    for (const std::string& file : executableFiles) {
        const std::string path = joinPath(runtimeDir, file);
        if (!chmodIfExists(path, 0755)) {
            error = "runtime chmod failed: " + path + ": " + std::strerror(errno);
            return false;
        }
    }

    return true;
}

static bool parseDrmModeLine(const std::string& line, int& width, int& height)
{
    const char* mode = std::strstr(line.c_str(), "mode: \"");
    if (!mode) return false;
    int w = 0;
    int h = 0;
    if (std::sscanf(mode, "mode: \"%dx%d\"", &w, &h) != 2) return false;
    if (w <= 0 || h <= 0) return false;
    width = w;
    height = h;
    return true;
}

static bool parseSizeLine(const std::string& line, int& width, int& height)
{
    int w = 0;
    int h = 0;
    if (std::sscanf(line.c_str(), "%dx%d", &w, &h) != 2) return false;
    if (w <= 0 || h <= 0) return false;
    width = w;
    height = h;
    return true;
}

static bool parseJsonIntField(const std::string& text, const std::string& key, int& value)
{
    const std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return false;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos >= text.size()) return false;
    char* end = nullptr;
    errno = 0;
    long parsed = std::strtol(text.c_str() + pos, &end, 10);
    if (errno != 0 || end == text.c_str() + pos) return false;
    value = static_cast<int>(parsed);
    return true;
}

static std::string parseJsonStringField(const std::string& text, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return "";
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    ++pos;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos >= text.size() || text[pos] != '"') return "";
    ++pos;
    std::string result;
    while (pos < text.size()) {
        char ch = text[pos++];
        if (ch == '"') break;
        if (ch == '\\' && pos < text.size()) {
            char escaped = text[pos++];
            switch (escaped) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            default: result += escaped; break;
            }
            continue;
        }
        result += ch;
    }
    return result;
}

static int normalizeRotationValue(int value, int fallback)
{
    value %= 360;
    if (value < 0) value += 360;
    if (value == 0 || value == 90 || value == 180 || value == 270) return value;
    return fallback;
}

static std::string currentDrmModeSpec()
{
    int width = 0;
    int height = 0;
    std::ifstream state("/sys/kernel/debug/dri/0/state");
    if (state.good()) {
        std::string line;
        while (std::getline(state, line)) {
            if (parseDrmModeLine(line, width, height))
                return std::to_string(width) + "x" + std::to_string(height);
        }
    }

    const std::vector<std::string> knownModeFiles = {
        "/sys/class/drm/card0-DSI-1/modes",
        "/sys/class/drm/card0-eDP-1/modes",
        "/sys/class/drm/card0-LVDS-1/modes",
        "/sys/class/drm/card0-HDMI-A-1/modes",
    };
    for (const std::string& path : knownModeFiles) {
        std::ifstream modeFile(path.c_str());
        std::string mode;
        if (modeFile.good() && std::getline(modeFile, mode) && parseSizeLine(mode, width, height))
            return std::to_string(width) + "x" + std::to_string(height);
    }

    DIR* dir = opendir("/sys/class/drm");
    if (!dir) return "";
    std::string result;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr && result.empty()) {
        const std::string name(entry->d_name);
        if (name.find("card") != 0 || name.find('-') == std::string::npos) continue;
        const std::string base = joinPath("/sys/class/drm", name);
        const std::string status = readFile(joinPath(base, "status"));
        if (!status.empty() && status.find("connected") == std::string::npos) continue;
        std::ifstream modeFile(joinPath(base, "modes").c_str());
        std::string mode;
        if (modeFile.good() && std::getline(modeFile, mode) && parseSizeLine(mode, width, height))
            result = std::to_string(width) + "x" + std::to_string(height);
    }
    closedir(dir);
    return result;
}

class JSBrowserPlayer : public JQBaseObject {
public:
    JSBrowserPlayer()
        : browserPid_(-1)
    {
    }

    ~JSBrowserPlayer() override = default;

    void prepareRuntime(JQFunctionInfo& info)
    {
        JSContext* ctx = info.GetContext();
        JSValueConst options = info.Length() > 0 ? info[0] : JS_UNDEFINED;
        const std::string workspace = getStringProperty(ctx, options, "workspace", "");
        const std::string dataDir = getStringProperty(ctx, options, "dataDir", "");
        if (workspace.empty()) {
            throwError(info, "workspace is empty");
            return;
        }

        if (!dataDir.empty())
            cleanupOldExtractedRuntime(dataDir);

        const std::string runtimeDir = joinPath(workspace, kRuntimeRelative);
        std::string error;
        if (!ensurePackagedRuntimeReady(runtimeDir, error)) {
            throwError(info, error);
            return;
        }

        publishState("runtime_ready", runtimeDir);
        info.GetReturnValue().Set(runtimeDir);
    }

    void startBrowser(JQFunctionInfo& info)
    {
        JSContext* ctx = info.GetContext();
        JSValueConst options = info.Length() > 0 ? info[0] : JS_UNDEFINED;
        const std::string runtimePath = getStringProperty(ctx, options, "runtimePath", "");
        const std::string workdir = normalizeWorkdir(getStringProperty(ctx, options, "workdir", ""));
        const std::string logPath = getStringProperty(ctx, options, "logPath", joinPath(workdir, "wpe-drm.log"));
        const std::string url = getStringProperty(ctx, options, "url", kDefaultUrl);
        const std::string viewport = getStringProperty(ctx, options, "viewport", kDefaultViewport);
        const std::string panelSize = getStringProperty(ctx, options, "panelSize", viewport);
        const std::string drmMode = getStringProperty(ctx, options, "drmMode", "");
        const std::string displaySource = getStringProperty(ctx, options, "displaySource", "unknown");
        const std::string browserMode = getStringProperty(ctx, options, "browserMode", "");
        const std::string touchDevice = getStringProperty(ctx, options, "touchDevice", "");
        const std::string gpuMode = getStringProperty(ctx, options, "gpuMode", "auto");
        const std::string drm = getStringProperty(ctx, options, "drm", "/dev/dri/card0");
        const int rotation = getIntProperty(ctx, options, "rotation", kDefaultRotation);
        const int touchRotation = getIntProperty(ctx, options, "touchRotation", rotation);
        const int touchOffsetX = getIntProperty(ctx, options, "touchOffsetX", 0);
        const int touchOffsetY = getIntProperty(ctx, options, "touchOffsetY", 0);
        const int fpsMax = getIntProperty(ctx, options, "fpsMax", 0);
        const bool useOverlay = getBoolProperty(ctx, options, "useOverlay", true);
        const int overlayZpos = getIntProperty(ctx, options, "overlayZpos", 3);

        if (runtimePath.empty()) {
            throwError(info, "runtimePath is empty");
            return;
        }
        std::string runtimeError;
        if (!ensurePackagedRuntimeReady(runtimePath, runtimeError)) {
            throwError(info, runtimeError);
            return;
        }
        const std::string launcher = joinPath(runtimePath, "run.sh");
        if (!pathExists(launcher)) {
            throwError(info, std::string("runtime run.sh missing: ") + launcher);
            return;
        }
        if (!pathExists(joinPath(runtimePath, "wpe-drm-minimal"))) {
            throwError(info, std::string("runtime binary missing: ") + joinPath(runtimePath, "wpe-drm-minimal"));
            return;
        }
        if (!ensureDirRecursive(workdir)) {
            throwError(info, std::string("mkdir failed: ") + workdir);
            return;
        }
        removeRecursive(joinPath(keyboardDirForWorkdir(workdir), "requests"));
        removeRecursive(joinPath(keyboardDirForWorkdir(workdir), "responses"));
        removeRecursive(joinPath(keyboardDirForWorkdir(workdir), "status"));
        if (!prepareKeyboardDirs(workdir)) {
            throwError(info, std::string("keyboard dir mkdir failed: ") + keyboardDirForWorkdir(workdir));
            return;
        }
        ensureDirRecursive(parentDir(logPath));

        const std::string pidFile = joinPath(workdir, "browser.pid");
        stopBrowserByPath(pidFile);
        cleanupScopedBrowserProcesses(runtimePath, workdir);

        pid_t pid = fork();
        if (pid < 0) {
            throwError(info, std::string("fork failed: ") + std::strerror(errno));
            return;
        }

        if (pid == 0) {
#ifdef __linux__
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (getppid() == 1) _exit(126);
#endif
            setpgid(0, 0);
            chdir(runtimePath.c_str());

            int logFd = open(logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
            if (logFd >= 0) {
                dup2(logFd, STDOUT_FILENO);
                dup2(logFd, STDERR_FILENO);
                if (logFd > STDERR_FILENO) close(logFd);
            }

            setenv("WPE_MESA_DIR", runtimePath.c_str(), 1);
            setenv("WPE_VAR_DIR", workdir.c_str(), 1);
            setenv("WPE_CHROME_STATE", joinPath(workdir, "browser-state.ini").c_str(), 1);
            setenv("WPE_CHROME_RENDER_STATE", joinPath(workdir, "chrome-render-state.ini").c_str(), 1);
            setenv("WPE_BROWSER_DB", joinPath(workdir, "browser.sqlite3").c_str(), 1);
            setenv("WPE_PROFILES_DIR", joinPath(workdir, "profiles").c_str(), 1);
            setenv("WPE_PROFILE_SWITCH_FILE", joinPath(workdir, "profile-switch.request").c_str(), 1);
            setenv("WPE_CHROME_FONT", joinPath(runtimePath, "assets/fonts/miniapp/NotoSansSC-Regular.otf").c_str(), 1);
            setenv("WPE_KEYBOARD_DIR", keyboardDirForWorkdir(workdir).c_str(), 1);
            setenv("WPE_DRM_RUNTIME_DIR", joinPath(workdir, "runtime-tmp").c_str(), 1);
            setenv("WPE_DRM_SKIP_MASTER", "1", 0);
            setenv("WPE_DRM_USE_OVERLAY", useOverlay ? "1" : "0", 1);
            const std::string overlayZposValue = std::to_string(overlayZpos);
            setenv("WPE_DRM_ZPOS", overlayZposValue.c_str(), 1);
            const std::string rotationValue = std::to_string(rotation);
            const std::string touchRotationValue = std::to_string(touchRotation);
            if (!panelSize.empty()) setenv("WPE_PANEL_SIZE", panelSize.c_str(), 1);
            if (!viewport.empty()) {
                setenv("WPE_VIEWPORT", viewport.c_str(), 1);
                setenv("WPE_DRM_VIEWPORT", viewport.c_str(), 1);
            }
            if (!drmMode.empty()) setenv("WPE_DRM_MODE", drmMode.c_str(), 1);
            if (!displaySource.empty()) setenv("WPE_DISPLAY_SOURCE", displaySource.c_str(), 1);
            if (!browserMode.empty()) setenv("WPE_BROWSER_MODE", browserMode.c_str(), 1);
            setenv("WPE_PANEL_ROTATION", rotationValue.c_str(), 1);
            setenv("WPE_TOUCH_ROTATION", touchRotationValue.c_str(), 1);
            if (!touchDevice.empty()) setenv("WPE_TOUCH_DEVICE", touchDevice.c_str(), 1);
            setenv("WPE_GPU_MODE", gpuMode.c_str(), 1);
            const std::string touchOffsetXValue = std::to_string(touchOffsetX);
            const std::string touchOffsetYValue = std::to_string(touchOffsetY);
            setenv("WPE_TOUCH_OFFSET_X", touchOffsetXValue.c_str(), 1);
            setenv("WPE_TOUCH_OFFSET_Y", touchOffsetYValue.c_str(), 1);
            if (fpsMax > 0) {
                const std::string fpsMaxValue = std::to_string(fpsMax);
                setenv("WPE_DRM_MAX_FPS", fpsMaxValue.c_str(), 1);
            }
            setenv("WPE_CHROME_LAYOUT", "inset", 0);
            setenv("GST_REGISTRY", joinPath(workdir, "gst-registry.bin").c_str(), 1);
            setenv("WPE_DEFAULT_URL", url.c_str(), 1);
            setenv("HOME", workdir.c_str(), 1);

            std::vector<std::string> args;
            args.push_back(launcher);
            args.push_back(url);
            args.push_back(drm);
            args.push_back(viewport);
            args.push_back(std::to_string(rotation));
            std::vector<char*> argv;
            for (std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);
            execv(launcher.c_str(), argv.data());
            _exit(127);
        }

        {
            std::lock_guard<std::mutex> lock(processMutex_);
            browserPid_ = pid;
            pidFile_ = pidFile;
            workdir_ = workdir;
            runtimePath_ = runtimePath;
        }
        writePidFile(pidFile, pid);
        publishState("running", "pid=" + std::to_string(static_cast<long>(pid)));
        info.GetReturnValue().Set(static_cast<int32_t>(pid));
    }

    void stopBrowser(JQFunctionInfo& info)
    {
        JSContext* ctx = info.GetContext();
        JSValueConst options = info.Length() > 0 ? info[0] : JS_UNDEFINED;
        const std::string workdir = normalizeWorkdir(getStringProperty(ctx, options, "workdir", ""));
        std::string pidFile;
        std::string runtimePath;
        std::string scopedWorkdir;
        pid_t pid = -1;
        {
            std::lock_guard<std::mutex> lock(processMutex_);
            pidFile = !workdir.empty() ? joinPath(workdir, "browser.pid") : pidFile_;
            runtimePath = runtimePath_;
            scopedWorkdir = !workdir.empty() ? workdir : workdir_;
            if (!pidFile.empty() && pidFile == pidFile_ && browserPid_ > 1) pid = browserPid_;
        }
        if (pid <= 1) pid = readPidFile(pidFile);

        // SIGTERM 同步发出;等待退出/升级 SIGKILL/残留进程清理放到后台线程,
        // 避免最坏 ~5s 的 usleep 循环阻塞 miniapp JS 线程(startBrowser 里的
        // 同步等待保持不变——新实例启动前必须确认旧实例释放 DRM)。
        if (pid > 1) {
            kill(-pid, SIGTERM);
            kill(pid, SIGTERM);
        }
        {
            std::lock_guard<std::mutex> lock(processMutex_);
            if (pidFile.empty() || pidFile == pidFile_) browserPid_ = -1;
        }
        std::thread([pid, pidFile, runtimePath, scopedWorkdir]() {
            if (pid > 1 && waitForProcessExit(pid, 2000) != 0) {
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                waitForProcessExit(pid, 1000);
            }
            if (!pidFile.empty()) unlink(pidFile.c_str());
            cleanupScopedBrowserProcesses(runtimePath, scopedWorkdir);
        }).detach();
        publishState("stopped", "ok");
        info.GetReturnValue().Set(true);
    }

    void isBrowserRunning(JQFunctionInfo& info)
    {
        JSContext* ctx = info.GetContext();
        JSValueConst options = info.Length() > 0 ? info[0] : JS_UNDEFINED;
        const std::string workdir = normalizeWorkdir(getStringProperty(ctx, options, "workdir", ""));
        std::string pidFile;
        pid_t pid = -1;
        {
            std::lock_guard<std::mutex> lock(processMutex_);
            pidFile = !workdir.empty() ? joinPath(workdir, "browser.pid") : pidFile_;
            pid = browserPid_;
        }
        if (pid <= 1) pid = readPidFile(pidFile);
        info.GetReturnValue().Set(processExists(pid));
    }

    void pollKeyboardRequest(JQFunctionInfo& info)
    {
        JSContext* ctx = info.GetContext();
        JSValueConst options = info.Length() > 0 ? info[0] : JS_UNDEFINED;
        const std::string workdir = normalizeWorkdir(getStringProperty(ctx, options, "workdir", ""));
        if (workdir.empty()) {
            info.GetReturnValue().Set("");
            return;
        }

        const std::string requestPath = joinPath(joinPath(keyboardDirForWorkdir(workdir), "requests"), "current.json");
        struct stat st;
        if (stat(requestPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            info.GetReturnValue().Set("");
            return;
        }

        info.GetReturnValue().Set(readFile(requestPath));
    }

    void pollKeyboardCompletion(JQFunctionInfo& info)
    {
        JSContext* ctx = info.GetContext();
        JSValueConst options = info.Length() > 0 ? info[0] : JS_UNDEFINED;
        const std::string workdir = normalizeWorkdir(getStringProperty(ctx, options, "workdir", ""));
        const std::string id = getStringProperty(ctx, options, "id", "");
        if (workdir.empty() || !isSafeKeyboardId(id)) {
            info.GetReturnValue().Set("");
            return;
        }
        const std::string statusPath = joinPath(
            joinPath(keyboardDirForWorkdir(workdir), "status"), id + ".json");
        struct stat st;
        if (stat(statusPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            info.GetReturnValue().Set("");
            return;
        }
        const std::string value = readFile(statusPath);
        unlink(statusPath.c_str());
        info.GetReturnValue().Set(value);
    }

    void respondKeyboardRequest(JQFunctionInfo& info)
    {
        JSContext* ctx = info.GetContext();
        JSValueConst options = info.Length() > 0 ? info[0] : JS_UNDEFINED;
        const std::string workdir = normalizeWorkdir(getStringProperty(ctx, options, "workdir", ""));
        const std::string id = getStringProperty(ctx, options, "id", "");
        const bool confirmed = getBoolProperty(ctx, options, "confirmed", false);
        const std::string text = getStringProperty(ctx, options, "text", "");
        const int sequence = std::max(0, getIntProperty(ctx, options, "sequence", 0));

        if (workdir.empty()) {
            throwError(info, "workdir is empty");
            return;
        }
        if (!isSafeKeyboardId(id)) {
            throwError(info, "invalid keyboard request id");
            return;
        }
        if (!prepareKeyboardDirs(workdir)) {
            throwError(info, std::string("keyboard dir mkdir failed: ") + keyboardDirForWorkdir(workdir));
            return;
        }

        const std::string keyboardDir = keyboardDirForWorkdir(workdir);
        const std::string responsesDir = joinPath(keyboardDir, "responses");
        const std::string responsePath = joinPath(responsesDir, id + (confirmed ? ".ok" : ".cancel"));
        std::ostringstream payload;
        payload << "{\"sequence\":" << sequence;
        if (confirmed) payload << ",\"text\":\"" << jsonEscape(text) << "\"";
        payload << "}";
        if (!writeFileAtomic(responsePath, payload.str(), 0600, true)) {
            throwError(info, std::string("keyboard response write failed: ") + responsePath + ": " + std::strerror(errno));
            return;
        }
        publishState("keyboard_response", id + (confirmed ? ":ok" : ":cancel"));
        info.GetReturnValue().Set(true);
    }

    void updateKeyboardRequest(JQFunctionInfo& info)
    {
        JSContext* ctx = info.GetContext();
        JSValueConst options = info.Length() > 0 ? info[0] : JS_UNDEFINED;
        const std::string workdir = normalizeWorkdir(getStringProperty(ctx, options, "workdir", ""));
        const std::string id = getStringProperty(ctx, options, "id", "");
        const std::string text = getStringProperty(ctx, options, "text", "");
        const int sequence = std::max(0, getIntProperty(ctx, options, "sequence", 0));

        if (workdir.empty()) {
            throwError(info, "workdir is empty");
            return;
        }
        if (!isSafeKeyboardId(id)) {
            throwError(info, "invalid keyboard request id");
            return;
        }
        if (!prepareKeyboardDirs(workdir)) {
            throwError(info, std::string("keyboard dir mkdir failed: ") + keyboardDirForWorkdir(workdir));
            return;
        }

        const std::string keyboardDir = keyboardDirForWorkdir(workdir);
        const std::string updatePath = joinPath(joinPath(keyboardDir, "responses"), id + ".update");
        std::ostringstream payload;
        payload << "{\"sequence\":" << sequence << ",\"text\":\"" << jsonEscape(text) << "\"}";
        if (!writeFileAtomic(updatePath, payload.str(), 0600, false)) {
            throwError(info, std::string("keyboard update write failed: ") + updatePath + ": " + std::strerror(errno));
            return;
        }
        publishState("keyboard_update", id);
        info.GetReturnValue().Set(true);
    }

    void getKeyboardProfile(JQFunctionInfo& info)
    {
        const std::string hostname = trimAscii(readFile("/etc/hostname"));
        const std::string osRelease = readFile("/etc/os-release");
        const std::string cfg = readFile("/etc/miniapp/resources/cfg.json");
        const std::string haystack = toLowerAscii(hostname + "\n" + osRelease + "\n" + cfg);

        std::string mode = "globalOnly";
        std::string reason = "default";
        if (haystack.find("y07") != std::string::npos) {
            mode = "textareaOnly";
            reason = "y07";
        } else if (haystack.find("3.14.") != std::string::npos &&
            haystack.find("input") != std::string::npos) {
            // 之前还接受子串 "im",但 time/limit/minimal 等词都含 "im",
            // 该分支实际恒为真;只保留 "input" 判定。
            mode = "textareaOnly";
            reason = "ime_3.14";
        }

        std::ostringstream ss;
        ss << "{";
        ss << "\"mode\":\"" << mode << "\",";
        ss << "\"reason\":\"" << reason << "\",";
        ss << "\"hostname\":\"" << jsonEscape(hostname) << "\",";
        ss << "\"source\":\"system\"";
        ss << "}";
        info.GetReturnValue().Set(ss.str());
    }

    void getSystemDisplayConfig(JQFunctionInfo& info)
    {
        const std::string cfg = readFile("/etc/miniapp/resources/cfg.json");
        int width = 0;
        int height = 0;
        int direction = kDefaultRotation;
        int videoDirection = -1;
        int touchDirection = -1;
        int touchOffsetX = 0;
        int touchOffsetY = 0;
        int fpsMax = 0;
        std::string touchDevice;

        if (!cfg.empty()) {
            parseJsonIntField(cfg, "width", width);
            parseJsonIntField(cfg, "height", height);
            parseJsonIntField(cfg, "direction", direction);
            parseJsonIntField(cfg, "video_direction", videoDirection);
            parseJsonIntField(cfg, "tp_direction", touchDirection);
            parseJsonIntField(cfg, "tp_xoffset", touchOffsetX);
            parseJsonIntField(cfg, "tp_yoffset", touchOffsetY);
            parseJsonIntField(cfg, "fps_max", fpsMax);
            touchDevice = parseJsonStringField(cfg, "tp");
        }

        direction = normalizeRotationValue(direction, kDefaultRotation);
        videoDirection = videoDirection >= 0 ? normalizeRotationValue(videoDirection, direction) : direction;
        touchDirection = touchDirection >= 0 ? normalizeRotationValue(touchDirection, videoDirection) : videoDirection;
        const std::string drmMode = currentDrmModeSpec();
        int panelWidth = width;
        int panelHeight = height;
        if ((videoDirection == 90 || videoDirection == 270) && panelWidth > 0 && panelHeight > 0 && panelWidth < panelHeight)
            std::swap(panelWidth, panelHeight);
        const std::string panelSize = panelWidth > 0 && panelHeight > 0
            ? std::to_string(panelWidth) + "x" + std::to_string(panelHeight)
            : "";

        std::ostringstream ss;
        ss << "{";
        ss << "\"source\":\"" << (cfg.empty() ? "missing" : "system_cfg") << "\",";
        ss << "\"width\":" << width << ",";
        ss << "\"height\":" << height << ",";
        ss << "\"panelSize\":\"" << jsonEscape(panelSize) << "\",";
        ss << "\"frameworkRotation\":" << direction << ",";
        ss << "\"videoRotation\":" << videoDirection << ",";
        ss << "\"touchRotation\":" << touchDirection << ",";
        ss << "\"touchOffsetX\":" << touchOffsetX << ",";
        ss << "\"touchOffsetY\":" << touchOffsetY << ",";
        ss << "\"fpsMax\":" << fpsMax << ",";
        ss << "\"drmMode\":\"" << jsonEscape(drmMode) << "\",";
        ss << "\"touchDevice\":\"" << jsonEscape(touchDevice) << "\"";
        ss << "}";
        info.GetReturnValue().Set(ss.str());
    }

    void getDrmScreenSize(JQFunctionInfo& info)
    {
        info.GetReturnValue().Set(currentDrmModeSpec());
    }

protected:
    void OnGCCollect() override
    {
    }

private:
    std::string normalizeWorkdir(const std::string& value)
    {
        if (!value.empty()) {
            if (value[value.size() - 1] == '/') return value.substr(0, value.size() - 1);
            return value;
        }
        std::lock_guard<std::mutex> lock(processMutex_);
        return workdir_;
    }

    void publishState(const std::string& state, const std::string& detail)
    {
        LOGI("%s state %s %s", kTag, state.c_str(), detail.c_str());
    }

    void publishError(const std::string& message)
    {
        LOGE("%s error %s", kTag, message.c_str());
    }

    void throwError(JQFunctionInfo& info, const std::string& message)
    {
        publishError(message);
        info.GetReturnValue().ThrowInternalError("%s", message.c_str());
    }

    void stopBrowserByPath(const std::string& pidFile)
    {
        pid_t pid = -1;
        {
            std::lock_guard<std::mutex> lock(processMutex_);
            if (!pidFile.empty() && pidFile == pidFile_ && browserPid_ > 1) pid = browserPid_;
        }
        if (pid <= 1) pid = readPidFile(pidFile);
        if (pid <= 1) {
            if (!pidFile.empty()) unlink(pidFile.c_str());
            std::lock_guard<std::mutex> lock(processMutex_);
            if (pidFile.empty() || pidFile == pidFile_) browserPid_ = -1;
            return;
        }

        kill(-pid, SIGTERM);
        kill(pid, SIGTERM);
        if (waitForProcessExit(pid, 2000) != 0) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitForProcessExit(pid, 1000);
        }

        if (!pidFile.empty()) unlink(pidFile.c_str());
        std::lock_guard<std::mutex> lock(processMutex_);
        if (pidFile.empty() || pidFile == pidFile_) browserPid_ = -1;
    }

    std::mutex processMutex_;
    pid_t browserPid_;
    std::string pidFile_;
    std::string workdir_;
    std::string runtimePath_;
};

static JSValue createBrowserPlayer(JQModuleEnv* env)
{
    JQFunctionTemplateRef tpl = JQFunctionTemplate::New(env, "browserPlayer");
    tpl->InstanceTemplate()->setObjectCreator([]() {
        static JSBrowserPlayer* player = []() {
            JSBrowserPlayer* instance = new JSBrowserPlayer();
            instance->REF();
            return instance;
        }();
        return player;
    });

    tpl->SetProtoMethod("prepareRuntime", &JSBrowserPlayer::prepareRuntime);
    tpl->SetProtoMethod("startBrowser", &JSBrowserPlayer::startBrowser);
    tpl->SetProtoMethod("stopBrowser", &JSBrowserPlayer::stopBrowser);
    tpl->SetProtoMethod("isBrowserRunning", &JSBrowserPlayer::isBrowserRunning);
    tpl->SetProtoMethod("pollKeyboardRequest", &JSBrowserPlayer::pollKeyboardRequest);
    tpl->SetProtoMethod("pollKeyboardCompletion", &JSBrowserPlayer::pollKeyboardCompletion);
    tpl->SetProtoMethod("updateKeyboardRequest", &JSBrowserPlayer::updateKeyboardRequest);
    tpl->SetProtoMethod("respondKeyboardRequest", &JSBrowserPlayer::respondKeyboardRequest);
    tpl->SetProtoMethod("getKeyboardProfile", &JSBrowserPlayer::getKeyboardProfile);
    tpl->SetProtoMethod("getSystemDisplayConfig", &JSBrowserPlayer::getSystemDisplayConfig);
    tpl->SetProtoMethod("getDrmScreenSize", &JSBrowserPlayer::getDrmScreenSize);
    return tpl->CallConstructor();
}

}  // namespace

void browser_init(JQModuleEnv* env)
{
    env->setModuleExport("browserPlayer", createBrowserPlayer(env));
}

}  // namespace browser
