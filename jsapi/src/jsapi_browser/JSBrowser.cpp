#include "jqutil_v2/jqutil.h"
#include "jqutil_v2/JQSignal.h"
#include "utils/log.h"

#include <atomic>
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
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#ifdef __linux__
#include <sys/prctl.h>
#endif

using namespace JQUTIL_NS;

namespace browser {
namespace {

const char* kTag = "miniapp-browser-launcher";
const char* kRuntimeArchiveRelative = "assets/wpe-drm2-runtime.tar.gz";
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

static bool ensureDir(const std::string& path)
{
    if (path.empty()) return false;
    if (mkdir(path.c_str(), 0777) == 0 || errno == EEXIST) {
        chmod(path.c_str(), 0777);
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

static bool writeFile(const std::string& path, const std::string& value, mode_t mode = 0666)
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
    if (fsync(fd) != 0) ok = false;
    close(fd);
    chmod(path.c_str(), mode);
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

static std::string shellSingleQuote(const std::string& value)
{
    std::string out("'");
    for (char ch : value) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    out += "'";
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
    return writeFile(path, std::to_string(static_cast<long>(pid)) + "\n", 0666);
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

static void cleanupScopedBrowserProcesses(const std::string& runtimePath, const std::string& workdir)
{
    if (runtimePath.empty() && workdir.empty()) return;

    DIR* proc = opendir("/proc");
    if (!proc) return;

    std::vector<pid_t> pids;
    struct dirent* entry = nullptr;
    while ((entry = readdir(proc)) != nullptr) {
        pid_t pid = parseProcPid(entry->d_name);
        if (pid <= 1 || pid == getpid()) continue;

        const std::string procDir = joinPath("/proc", entry->d_name);
        std::string cmdline = readFile(joinPath(procDir, "cmdline"));
        if (cmdline.empty() || !isBrowserProcessName(cmdline)) continue;

        std::string environ = readFile(joinPath(procDir, "environ"));
        const bool scoped = containsString(cmdline, runtimePath) ||
            containsString(environ, runtimePath) ||
            containsString(cmdline, workdir) ||
            containsString(environ, workdir);
        if (scoped) pids.push_back(pid);
    }
    closedir(proc);

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

static int runTarExtract(const std::string& archive, const std::string& dest)
{
    const std::string sentinel = joinPath(dest, ".extract-ok");
    unlink(sentinel.c_str());

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        const std::string command = "gzip -dc " + shellSingleQuote(archive) +
            " | tar -xf - -C " + shellSingleQuote(dest) +
            " && touch " + shellSingleQuote(sentinel);
        execlp("sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        if (errno == ECHILD) {
            for (int i = 0; i < 7200 && processExists(pid); ++i)
                usleep(100000);
            return pathExists(sentinel) ? 0 : -1;
        }
        return -1;
    }
    if (!WIFEXITED(status)) return -1;
    int exitStatus = WEXITSTATUS(status);
    if (exitStatus == 0 && !pathExists(sentinel)) return -1;
    return exitStatus;
}

static std::string archiveStamp(const std::string& archive)
{
    struct stat st;
    if (stat(archive.c_str(), &st) != 0) return "";
    std::ostringstream ss;
    ss << "archive=" << archive << "\n";
    ss << "size=" << static_cast<long long>(st.st_size) << "\n";
    ss << "mtime=" << static_cast<long long>(st.st_mtime) << "\n";
    return ss.str();
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

class JSBrowserPlayer : public JQBaseObject {
public:
    JSBrowserPlayer()
        : browserPid_(-1)
    {
    }

    ~JSBrowserPlayer() override = default;

    JQSignal<std::string> onStateChanged;
    JQSignal<std::string> onError;

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
        if (dataDir.empty()) {
            throwError(info, "dataDir is empty");
            return;
        }

        const std::string archive = joinPath(workspace, kRuntimeArchiveRelative);
        if (!pathExists(archive)) {
            throwError(info, std::string("runtime archive missing: ") + archive);
            return;
        }

        const std::string browserDir = joinPath(dataDir, "browser");
        const std::string runtimeDir = joinPath(browserDir, "runtime");
        const std::string marker = joinPath(browserDir, ".installed.json");
        const std::string stamp = archiveStamp(archive);
        if (stamp.empty()) {
            throwError(info, std::string("runtime archive stat failed: ") + archive);
            return;
        }

        if (pathExists(joinPath(runtimeDir, "run.sh")) &&
            pathExists(joinPath(runtimeDir, "wpe-drm-minimal")) &&
            readFile(marker) == stamp) {
            publishState("runtime_ready", runtimeDir);
            info.GetReturnValue().Set(runtimeDir);
            return;
        }

        stopBrowserByPath(joinPath(browserDir, "browser.pid"));
        cleanupScopedBrowserProcesses(runtimeDir, browserDir);

        if (!ensureDirRecursive(browserDir)) {
            throwError(info, std::string("mkdir failed: ") + browserDir);
            return;
        }

        const std::string tmpDir = joinPath(browserDir, "runtime.tmp." + std::to_string(static_cast<long>(getpid())));
        const std::string oldDir = joinPath(browserDir, "runtime.old");
        removeRecursive(tmpDir);
        removeRecursive(oldDir);
        if (!ensureDirRecursive(tmpDir)) {
            throwError(info, std::string("mkdir failed: ") + tmpDir);
            return;
        }

        publishState("runtime_extracting", archive);
        int tarStatus = runTarExtract(archive, tmpDir);
        if (tarStatus != 0) {
            removeRecursive(tmpDir);
            throwError(info, "runtime extract failed status=" + std::to_string(tarStatus));
            return;
        }

        std::string extractedRoot = tmpDir;
        if (!pathExists(joinPath(extractedRoot, "run.sh"))) {
            const std::string nested = joinPath(tmpDir, "wpe-drm2");
            if (pathExists(joinPath(nested, "run.sh"))) {
                extractedRoot = nested;
            } else {
                removeRecursive(tmpDir);
                throwError(info, "runtime run.sh missing after extract");
                return;
            }
        }
        if (!pathExists(joinPath(extractedRoot, "wpe-drm-minimal"))) {
            removeRecursive(tmpDir);
            throwError(info, "runtime wpe-drm-minimal missing after extract");
            return;
        }

        if (pathExists(runtimeDir) && rename(runtimeDir.c_str(), oldDir.c_str()) != 0) {
            removeRecursive(tmpDir);
            throwError(info, std::string("rename old runtime failed: ") + std::strerror(errno));
            return;
        }
        if (rename(extractedRoot.c_str(), runtimeDir.c_str()) != 0) {
            if (pathExists(oldDir)) rename(oldDir.c_str(), runtimeDir.c_str());
            removeRecursive(tmpDir);
            throwError(info, std::string("install runtime failed: ") + std::strerror(errno));
            return;
        }
        removeRecursive(tmpDir);
        removeRecursive(oldDir);
        chmod(joinPath(runtimeDir, "run.sh").c_str(), 0777);
        chmod(joinPath(runtimeDir, "wpe-drm-minimal").c_str(), 0777);
        writeFile(marker, stamp, 0666);

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
        const std::string drm = getStringProperty(ctx, options, "drm", "/dev/dri/card0");
        const int rotation = getIntProperty(ctx, options, "rotation", kDefaultRotation);

        if (runtimePath.empty()) {
            throwError(info, "runtimePath is empty");
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

            int logFd = open(logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0666);
            if (logFd >= 0) {
                dup2(logFd, STDOUT_FILENO);
                dup2(logFd, STDERR_FILENO);
                if (logFd > STDERR_FILENO) close(logFd);
            }

            setenv("WPE_MESA_DIR", runtimePath.c_str(), 1);
            setenv("WPE_VAR_DIR", workdir.c_str(), 1);
            setenv("WPE_CHROME_STATE", joinPath(workdir, "browser-state.ini").c_str(), 1);
            setenv("WPE_CHROME_RENDER_STATE", joinPath(workdir, "chrome-render-state.ini").c_str(), 1);
            setenv("WPE_DRM_RUNTIME_DIR", joinPath(workdir, "runtime-tmp").c_str(), 1);
            setenv("WPE_DRM_SKIP_MASTER", "1", 0);
            setenv("WPE_DRM_USE_OVERLAY", "1", 0);
            setenv("WPE_DRM_ZPOS", "3", 0);
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
            logPath_ = logPath;
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
        {
            std::lock_guard<std::mutex> lock(processMutex_);
            pidFile = !workdir.empty() ? joinPath(workdir, "browser.pid") : pidFile_;
            runtimePath = runtimePath_;
            scopedWorkdir = !workdir.empty() ? workdir : workdir_;
        }
        stopBrowserByPath(pidFile);
        cleanupScopedBrowserProcesses(runtimePath, scopedWorkdir);
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

    void getDrmScreenSize(JQFunctionInfo& info)
    {
        int width = 0;
        int height = 0;
        std::ifstream state("/sys/kernel/debug/dri/0/state");
        if (state.good()) {
            std::string line;
            while (std::getline(state, line)) {
                if (parseDrmModeLine(line, width, height)) {
                    info.GetReturnValue().Set(std::to_string(width) + "x" + std::to_string(height));
                    return;
                }
            }
        }
        std::ifstream dsiMode("/sys/class/drm/card0-DSI-1/modes");
        if (dsiMode.good()) {
            std::string mode;
            if (std::getline(dsiMode, mode)) {
                if (std::sscanf(mode.c_str(), "%dx%d", &width, &height) == 2 && width > 0 && height > 0) {
                    info.GetReturnValue().Set(std::to_string(width) + "x" + std::to_string(height));
                    return;
                }
            }
        }
        info.GetReturnValue().Set("");
    }

    void getState(JQFunctionInfo& info)
    {
        std::string payload;
        {
            std::lock_guard<std::mutex> lock(processMutex_);
            payload = makeStatePayloadLocked();
        }
        info.GetReturnValue().Set(payload);
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

    std::string makeStatePayloadLocked()
    {
        pid_t pid = browserPid_ > 1 ? browserPid_ : readPidFile(pidFile_);
        bool running = processExists(pid);
        std::ostringstream ss;
        ss << "{";
        ss << "\"state\":\"" << jsonEscape(lastState_) << "\",";
        ss << "\"detail\":\"" << jsonEscape(lastDetail_) << "\",";
        ss << "\"running\":" << (running ? "true" : "false") << ",";
        ss << "\"pid\":" << static_cast<long>(running ? pid : -1) << ",";
        ss << "\"runtimePath\":\"" << jsonEscape(runtimePath_) << "\",";
        ss << "\"workdir\":\"" << jsonEscape(workdir_) << "\",";
        ss << "\"logPath\":\"" << jsonEscape(logPath_) << "\"";
        ss << "}";
        return ss.str();
    }

    void publishState(const std::string& state, const std::string& detail)
    {
        std::string payload;
        {
            std::lock_guard<std::mutex> lock(processMutex_);
            lastState_ = state;
            lastDetail_ = detail;
            payload = makeStatePayloadLocked();
        }
        LOGI("%s state %s %s", kTag, state.c_str(), detail.c_str());
        onStateChanged.emit(payload);
    }

    void publishError(const std::string& message)
    {
        {
            std::lock_guard<std::mutex> lock(processMutex_);
            lastState_ = "error";
            lastDetail_ = message;
        }
        LOGE("%s error %s", kTag, message.c_str());
        onError.emit(message);
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
    std::string logPath_;
    std::string lastState_ = "idle";
    std::string lastDetail_;
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
    tpl->SetProtoMethod("getDrmScreenSize", &JSBrowserPlayer::getDrmScreenSize);
    tpl->SetProtoMethod("getState", &JSBrowserPlayer::getState);
    tpl->PrototypeTemplate()->Set("onStateChanged", &JSBrowserPlayer::onStateChanged);
    tpl->PrototypeTemplate()->Set("onError", &JSBrowserPlayer::onError);
    return tpl->CallConstructor();
}

}  // namespace

void browser_init(JQModuleEnv* env)
{
    env->setModuleExport("browserPlayer", createBrowserPlayer(env));
}

}  // namespace browser
