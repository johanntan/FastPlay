// Running helper programs with posix_spawn and separate output pipes.
#include "subprocess.h"
#include "utils.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

bool RunProcessCapture(const std::wstring& program, const std::vector<std::wstring>& args,
	std::string& output, std::string* errors, int* exitCode, const ProcessOptions& options) {
	output.clear();
	if (errors) errors->clear();
	if (exitCode) *exitCode = -1;

	std::vector<std::string> argStrings{WideToUtf8(program)};
	for (const auto& arg : args) argStrings.push_back(WideToUtf8(arg));
	std::vector<char*> argv;
	for (auto& arg : argStrings) argv.push_back(arg.data());
	argv.push_back(nullptr);

	std::vector<std::string> envStrings;
	std::vector<char*> envPointers;
	char** childEnv = environ;
	if (!options.pathDirectories.empty()) {
		std::string path;
		for (char** entry = environ; *entry; ++entry) {
			std::string value(*entry);
			if (value.rfind("PATH=", 0) == 0) path = value.substr(5);
			else envStrings.push_back(value);
		}
		for (const auto& dir : options.pathDirectories) {
			if (!path.empty()) path += ':';
			path += WideToUtf8(dir);
		}
		envStrings.push_back("PATH=" + path);
		for (auto& entry : envStrings) envPointers.push_back(entry.data());
		envPointers.push_back(nullptr);
		childEnv = envPointers.data();
	}

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
	if (errors) posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
	else posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addclose(&actions, outPipe[1]);
	if (errors) posix_spawn_file_actions_addclose(&actions, errPipe[1]);

	posix_spawnattr_t attributes;
	posix_spawnattr_init(&attributes);
	if (options.timeoutMs > 0) {
		posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
		posix_spawnattr_setpgroup(&attributes, 0);
	}
	pid_t pid = 0;
	// Bare names use the parent's PATH; YouTube installed tools use absolute paths.
	int err = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), childEnv);
	posix_spawnattr_destroy(&attributes);
	posix_spawn_file_actions_destroy(&actions);
	close(outPipe[1]);
	if (errors) close(errPipe[1]);
	if (err != 0) {
		close(outPipe[0]);
		if (errors) close(errPipe[0]);
		return false;
	}

	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs);
	auto remaining = [&]() {
		if (options.timeoutMs <= 0) return -1;
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		return static_cast<int>(std::max<int64_t>(0, ms));
	};
	bool timedOut = false;
	struct pollfd fds[2] = {{outPipe[0], POLLIN, 0}, {errors ? errPipe[0] : -1, POLLIN, 0}};
	std::string* sinks[2] = {&output, errors};
	int openPipes = errors ? 2 : 1;
	char buffer[4096];
	while (openPipes > 0) {
		int waitMs = remaining();
		if (waitMs == 0) {
			timedOut = true;
			break;
		}
		if (poll(fds, 2, waitMs) < 0) {
			if (errno == EINTR) continue;
			break;
		}
		for (int i = 0; i < 2; i++) {
			if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
			ssize_t n = read(fds[i].fd, buffer, sizeof(buffer));
			if (n > 0) sinks[i]->append(buffer, static_cast<size_t>(n));
			else if (n == 0 || errno != EINTR) {
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
	if (options.timeoutMs > 0) {
		while (!timedOut) {
			pid_t result = waitpid(pid, &status, WNOHANG);
			if (result == pid) break;
			if (result < 0 && errno != EINTR) return true;
			int waitMs = remaining();
			if (waitMs == 0) timedOut = true;
			else poll(nullptr, 0, std::min(waitMs, 10));
		}
		if (timedOut) {
			kill(-pid, SIGKILL); // The timed check and any helpers it spawned.
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
		}
	} else {
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	}
	if (exitCode) *exitCode = timedOut ? -2 : WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return true;
}
