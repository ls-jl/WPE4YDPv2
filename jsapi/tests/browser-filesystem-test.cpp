#include "BrowserFilesystem.h"

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <limits.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

int main()
{
    const char* temporaryBase = std::getenv("TMPDIR");
    if (!temporaryBase || !temporaryBase[0])
        temporaryBase = "/tmp";
    char resolvedBase[PATH_MAX];
    assert(realpath(temporaryBase, resolvedBase));
    std::string rootPattern = std::string(resolvedBase) + "/wpe-jsapi-fs-XXXXXX";
    std::vector<char> rootTemplate(rootPattern.begin(), rootPattern.end());
    rootTemplate.push_back('\0');
    char* root = mkdtemp(rootTemplate.data());
    assert(root);
    assert(chmod(root, 0751) == 0);

    std::string leaf = std::string(root) + "/browser/keyboard";
    std::string error;
    assert(browser::filesystem::ensureDirectoryTree(leaf, 0700, &error));

    struct stat status;
    assert(lstat(root, &status) == 0);
    assert((status.st_mode & 0777) == 0751);
    assert(lstat(leaf.c_str(), &status) == 0);
    assert((status.st_mode & 0777) == 0700);

    std::string link = std::string(root) + "/link";
    assert(symlink("/tmp", link.c_str()) == 0);
    assert(!browser::filesystem::ensureDirectoryTree(link + "/child", 0700, &error));

    std::string browserRoot = std::string(root) + "/browser";
    assert(browser::filesystem::readDisplayMode(browserRoot) == "native");
    assert(browser::filesystem::writeDisplayMode(browserRoot, "rotate90", &error));
    assert(browser::filesystem::readDisplayMode(browserRoot) == "rotate90");
    assert(lstat((browserRoot + "/display-mode").c_str(), &status) == 0);
    assert((status.st_mode & 0777) == 0600);
    assert(!browser::filesystem::writeDisplayMode(browserRoot, "rotate45", &error));
    assert(browser::filesystem::readDisplayMode(browserRoot) == "rotate90");

    {
        std::ofstream invalid((browserRoot + "/display-mode").c_str(),
            std::ios::out | std::ios::trunc);
        invalid << "invalid\n";
    }
    assert(browser::filesystem::readDisplayMode(browserRoot) == "native");

    unlink(link.c_str());
    unlink((browserRoot + "/display-mode").c_str());
    rmdir(leaf.c_str());
    rmdir((std::string(root) + "/browser").c_str());
    rmdir(root);
    return 0;
}
