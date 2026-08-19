#include "BrowserProcessIdentity.h"

#include <cassert>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

int main()
{
#ifndef __linux__
    browser::process::Identity fixture;
    fixture.pid = 42;
    fixture.processGroup = 42;
    fixture.startTime = 123456;
    fixture.runtimePath = "/runtime";
    fixture.workdir = "/workdir";
    browser::process::Identity parsed;
    assert(browser::process::parseIdentity(browser::process::serializeIdentity(fixture), parsed));
    assert(parsed.pid == fixture.pid);
    assert(parsed.processGroup == fixture.processGroup);
    assert(parsed.startTime == fixture.startTime);
    assert(!browser::process::parseIdentity("123\n", parsed));
    return 0;
#else
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        setpgid(0, 0);
        for (;;)
            pause();
    }
    setpgid(child, child);

    browser::process::Identity identity;
    bool captured = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (browser::process::captureIdentity(child, "/runtime", "/workdir", identity)) {
            captured = true;
            break;
        }
        usleep(10000);
    }
    assert(captured);
    assert(browser::process::identityMatchesRunningProcess(identity));

    browser::process::Identity parsed;
    assert(browser::process::parseIdentity(browser::process::serializeIdentity(identity), parsed));
    assert(parsed.pid == identity.pid);
    assert(parsed.processGroup == identity.processGroup);
    assert(parsed.startTime == identity.startTime);
    assert(parsed.runtimePath == "/runtime");
    assert(parsed.workdir == "/workdir");

    parsed.startTime++;
    assert(!browser::process::identityMatchesRunningProcess(parsed));
    assert(!browser::process::parseIdentity("123\n", parsed));

    kill(child, SIGTERM);
    bool zombieRejected = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (!browser::process::identityMatchesRunningProcess(identity)) {
            zombieRejected = true;
            break;
        }
        usleep(10000);
    }
    assert(zombieRejected);
    waitpid(child, nullptr, 0);
    assert(!browser::process::identityMatchesRunningProcess(identity));
    return 0;
#endif
}
