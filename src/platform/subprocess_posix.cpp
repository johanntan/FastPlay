// Running helper programs on macOS and other POSIX systems: posix_spawn with pipes for
// the output.

#include "subprocess.h"
#include "utils.h"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

bool RunProcessCapture(const std::wstring& program, const std::vector<std::wstring>& args,
                       std::string& output, std::string* errors, int* exitCode) {
    output.clear();
    if (errors) errors->clear();

    std::vector<std::string> argStrings;
    argStrings.push_back(WideToUtf8(program));
    for (const auto& arg : args) argStrings.push_back(WideToUtf8(arg));
    std::vector<char*> argv;
    for (auto& arg : argStrings) argv.push_back(&arg[0]);
    argv.push_back(nullptr);

    int outPipe[2];
    if (pipe(outPipe) != 0) return false;
    int errPipe[2] = {-1, -1};
    if (errors && pipe(errPipe) != 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        return false;
    }
    fcntl(outPipe[0], F_SETFD, FD_CLOEXEC);
    if (errors) fcntl(errPipe[0], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    if (errors) {
        posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    posix_spawn_file_actions_addclose(&actions, outPipe[1]);
    if (errors) posix_spawn_file_actions_addclose(&actions, errPipe[1]);

    pid_t pid = 0;
    // A bare name ("yt-dlp") is looked up on PATH; a path is used as given.
    int err = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(outPipe[1]);
    if (errors) close(errPipe[1]);
    if (err != 0) {
        close(outPipe[0]);
        if (errors) close(errPipe[0]);
        return false;
    }

    // Read both pipes as data arrives, so neither can fill up and stall the program.
    struct pollfd fds[2] = {{outPipe[0], POLLIN, 0}, {errors ? errPipe[0] : -1, POLLIN, 0}};
    std::string* sinks[2] = {&output, errors};
    int openPipes = errors ? 2 : 1;
    char buffer[4096];
    while (openPipes > 0) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(fds[i].fd, buffer, sizeof(buffer));
            if (n > 0) {
                sinks[i]->append(buffer, static_cast<size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                close(fds[i].fd);
                fds[i].fd = -1;
                openPipes--;
            }
        }
    }
    for (auto& fd : fds) {
        if (fd.fd >= 0) close(fd.fd);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (exitCode) *exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return true;
}
