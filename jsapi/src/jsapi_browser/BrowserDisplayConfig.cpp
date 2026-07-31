#include "BrowserDisplayConfig.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <vector>

namespace browser {
namespace display {
namespace {

std::string joinPath(const std::string& base, const std::string& name)
{
    if (base.empty()) return name;
    return base[base.size() - 1] == '/' ? base + name : base + "/" + name;
}

std::string readFile(const std::string& path)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input.good()) return "";
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

bool parseDrmModeLine(const std::string& line, int& width, int& height)
{
    const char* mode = std::strstr(line.c_str(), "mode: \"");
    if (!mode) return false;
    int parsedWidth = 0;
    int parsedHeight = 0;
    if (std::sscanf(mode, "mode: \"%dx%d\"", &parsedWidth, &parsedHeight) != 2
            || parsedWidth <= 0 || parsedHeight <= 0)
        return false;
    width = parsedWidth;
    height = parsedHeight;
    return true;
}

bool parseSizeLine(const std::string& line, int& width, int& height)
{
    int parsedWidth = 0;
    int parsedHeight = 0;
    if (std::sscanf(line.c_str(), "%dx%d", &parsedWidth, &parsedHeight) != 2
            || parsedWidth <= 0 || parsedHeight <= 0)
        return false;
    width = parsedWidth;
    height = parsedHeight;
    return true;
}

} // namespace

bool parseJsonIntField(const std::string& text, const std::string& key, int& value)
{
    const std::string needle = "\"" + key + "\"";
    size_t position = text.find(needle);
    if (position == std::string::npos) return false;
    position = text.find(':', position + needle.size());
    if (position == std::string::npos) return false;
    position++;
    while (position < text.size()
            && std::isspace(static_cast<unsigned char>(text[position])))
        position++;
    if (position >= text.size()) return false;
    char* end = nullptr;
    errno = 0;
    long parsed = std::strtol(text.c_str() + position, &end, 10);
    if (errno || end == text.c_str() + position) return false;
    value = static_cast<int>(parsed);
    return true;
}

std::string parseJsonStringField(const std::string& text, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    size_t position = text.find(needle);
    if (position == std::string::npos) return "";
    position = text.find(':', position + needle.size());
    if (position == std::string::npos) return "";
    position++;
    while (position < text.size()
            && std::isspace(static_cast<unsigned char>(text[position])))
        position++;
    if (position >= text.size() || text[position] != '"') return "";
    position++;
    std::string result;
    while (position < text.size()) {
        char character = text[position++];
        if (character == '"') break;
        if (character == '\\' && position < text.size()) {
            char escaped = text[position++];
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
        } else
            result += character;
    }
    return result;
}

int normalizeRotationValue(int value, int fallback)
{
    value %= 360;
    if (value < 0) value += 360;
    return value == 0 || value == 90 || value == 180 || value == 270
        ? value : fallback;
}

std::string currentDrmModeSpec()
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
        if (modeFile.good() && std::getline(modeFile, mode)
                && parseSizeLine(mode, width, height))
            return std::to_string(width) + "x" + std::to_string(height);
    }

    DIR* directory = opendir("/sys/class/drm");
    if (!directory) return "";
    std::string result;
    while (dirent* entry = readdir(directory)) {
        if (!result.empty()) break;
        const std::string name(entry->d_name);
        if (name.find("card") != 0 || name.find('-') == std::string::npos)
            continue;
        const std::string base = joinPath("/sys/class/drm", name);
        const std::string status = readFile(joinPath(base, "status"));
        if (!status.empty() && status.find("connected") == std::string::npos)
            continue;
        std::ifstream modeFile(joinPath(base, "modes").c_str());
        std::string mode;
        if (modeFile.good() && std::getline(modeFile, mode)
                && parseSizeLine(mode, width, height))
            result = std::to_string(width) + "x" + std::to_string(height);
    }
    closedir(directory);
    return result;
}

} // namespace display
} // namespace browser
