// Running helper programs on macOS and other POSIX systems: posix_spawn with a pipe for
// the output.

#include "subprocess.h"
#include "utils.h"

#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

bool RunProcessCapture(const std::wstring& program, const std::vector<std::wstring>& args,
                       std::string& output) {
    output.clear();

    std::vector<std::string> argStrings;
    argStrings.push_back(WideToUtf8(program));
    for (const auto& arg : args) argStrings.push_back(WideToUtf8(arg));
    std::vector<char*> argv;
    for (auto& arg : argStrings) argv.push_back(&arg[0]);
    argv.push_back(nullptr);

    int fds[2];
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

    pid_t pid = 0;
    // A bare name ("yt-dlp") is looked up on PATH; a path is used as given.
    int err = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (err != 0) {
        close(fds[0]);
        return false;
    }

    char buffer[4096];
    for (;;) {
        ssize_t n = read(fds[0], buffer, sizeof(buffer));
        if (n > 0) {
            output.append(buffer, static_cast<size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return true;
}
