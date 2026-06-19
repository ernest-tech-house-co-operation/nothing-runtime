// util/subprocess.cpp — cross-platform fork/exec (POSIX) and
// CreateProcess (Windows) wrappers.
//
// The public API in subprocess.hpp is identical on both platforms. Only
// the implementation switches on `#ifdef _WIN32`.
//
// POSIX path: fork + execvp + pipe + waitpid, exactly as before.
// Windows path: CreateProcessW + CreatePipe + WaitForSingleObject.
//
// Windows notes:
//   - argv is converted to a single quoted command line string for
//     CreateProcessW. Filenames with embedded double-quotes are handled
//     by backslash-escaping (rare in practice).
//   - PATH lookup is done with the same algorithm as the POSIX path,
//     but checking ".exe", ".bat", ".cmd" extensions if the raw name
//     doesn't have one. CreateProcess itself also does this, but
//     resolving explicitly gives us cleaner "command not found" errors.
//   - Captured stdout/stderr is read out of anonymous pipes in a loop
//     until both write ends are closed (signaled by ReadFile returning
//     ERROR_BROKEN_PIPE).

#include "subprocess.hpp"
#include "strings.hpp"

#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <vector>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <process.h>
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <array>
#endif

namespace nth::util {

#ifdef _WIN32

// --- Windows ---------------------------------------------------------------

namespace {

// Quote a single argument for the Windows command line. Wraps in double
// quotes if it contains spaces or tabs; backslash-escapes any embedded
// double quotes. We follow the CommandLineToArgvW conventions.
std::string quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string out = "\"";
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == '"') out.push_back('\\');
        out.push_back(a[i]);
    }
    out.push_back('"');
    return out;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                  nullptr, 0);
    std::wstring out(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

// Check whether `path` is executable on Windows. Considers the standard
// executable extensions if no extension is given.
bool is_executable_file(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return false;
    return true;
}

} // namespace

std::optional<std::string> resolve_on_path(const std::string& cmd) {
    // If it has a path separator, treat as direct.
    if (cmd.find('/') != std::string::npos || cmd.find('\\') != std::string::npos) {
        if (is_executable_file(cmd)) return cmd;
        // Try with .exe appended
        if (cmd.size() >= 4 && _stricmp(cmd.c_str() + cmd.size() - 4, ".exe") != 0) {
            std::string with_exe = cmd + ".exe";
            if (is_executable_file(with_exe)) return with_exe;
        }
        return std::nullopt;
    }
    // PATH lookup — separator is ';' on Windows.
    const char* path_env = std::getenv("PATH");
    if (!path_env) {
        // Try PATHEXT too — but if PATH is missing entirely we can't do
        // anything useful.
        return std::nullopt;
    }
    // Extensions to try if the name doesn't already have one.
    const char* exts_env = std::getenv("PATHEXT");
    std::vector<std::string> exts;
    if (exts_env) {
        std::string s(exts_env);
        size_t i = 0;
        while (i < s.size()) {
            size_t j = s.find(';', i);
            std::string e = (j == std::string::npos) ? s.substr(i) : s.substr(i, j - i);
            i = (j == std::string::npos) ? s.size() : j + 1;
            if (!e.empty()) exts.push_back(e);
        }
    } else {
        exts = {".exe", ".bat", ".cmd"};
    }
    // Walk PATH.
    std::string s(path_env);
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(';', i);
        std::string dir = (j == std::string::npos) ? s.substr(i) : s.substr(i, j - i);
        i = (j == std::string::npos) ? s.size() : j + 1;
        if (dir.empty()) continue;
        // Try with each extension.
        bool has_ext = cmd.find('.') != std::string::npos;
        if (has_ext) {
            std::string candidate = dir + "\\" + cmd;
            if (is_executable_file(candidate)) return candidate;
        } else {
            for (auto& ext : exts) {
                std::string candidate = dir + "\\" + cmd + ext;
                if (is_executable_file(candidate)) return candidate;
            }
        }
    }
    return std::nullopt;
}

namespace {

SubprocessResult run_with_capture(const std::vector<std::string>& argv, bool capture) {
    SubprocessResult r;
    if (argv.empty()) {
        r.error_message = "empty argv";
        return r;
    }
    auto resolved = resolve_on_path(argv[0]);
    if (!resolved) {
        r.error_message = "command not found on PATH: " + argv[0];
        return r;
    }
    r.started = true;

    // Build the command line.
    std::string cmdline;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmdline.push_back(' ');
        // First arg: use the resolved path so CreateProcess can find it.
        cmdline += quote_arg(i == 0 ? *resolved : argv[i]);
    }
    std::wstring wcmdline = to_wide(cmdline);

    HANDLE child_out_r = nullptr, child_out_w = nullptr;
    HANDLE child_err_r = nullptr, child_err_w = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    if (capture) {
        if (!CreatePipe(&child_out_r, &child_out_w, &sa, 0)
            || !CreatePipe(&child_err_r, &child_err_w, &sa, 0)) {
            r.error_message = "CreatePipe failed";
            return r;
        }
        // Ensure the read ends are NOT inherited by the child.
        SetHandleInformation(child_out_r, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(child_err_r, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (capture) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = child_out_w;
        si.hStdError  = child_err_w;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    }

    if (!CreateProcessW(nullptr,                  // lpApplicationName (null = parse cmdline)
                        const_cast<LPWSTR>(wcmdline.c_str()),
                        nullptr, nullptr,         // process/thread attrs
                        TRUE,                     // inherit handles
                        0,                        // creation flags
                        nullptr, nullptr,         // env, cwd
                        &si, &pi)) {
        DWORD err = GetLastError();
        if (capture) {
            if (child_out_r) CloseHandle(child_out_r);
            if (child_out_w) CloseHandle(child_out_w);
            if (child_err_r) CloseHandle(child_err_r);
            if (child_err_w) CloseHandle(child_err_w);
        }
        r.error_message = "CreateProcessW failed: error " + std::to_string(err);
        r.started = false;
        return r;
    }

    // Close child-side pipe handles in the parent so reads will terminate.
    if (capture) {
        CloseHandle(child_out_w);
        CloseHandle(child_err_w);
    }

    // Drain pipes.
    if (capture) {
        auto drain = [](HANDLE h, std::string& sink) {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
                sink.append(buf, n);
            }
        };
        // Read sequentially. The child writes to both pipes; we may block
        // on stdout while stderr fills up, but for our use case (test
        // runners etc.) output volumes are small enough that the OS pipe
        // buffer (64 KB) is sufficient.
        drain(child_out_r, r.stdout_text);
        drain(child_err_r, r.stderr_text);
        CloseHandle(child_out_r);
        CloseHandle(child_err_r);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = (int)code;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

} // namespace

SubprocessResult run_subprocess(const std::vector<std::string>& argv) {
    return run_with_capture(argv, /*capture=*/true);
}

SubprocessResult run_subprocess_inherit(const std::vector<std::string>& argv) {
    return run_with_capture(argv, /*capture=*/false);
}

#else // _WIN32

// --- POSIX -----------------------------------------------------------------

std::optional<std::string> resolve_on_path(const std::string& cmd) {
    if (cmd.find('/') != std::string::npos) {
        if (access(cmd.c_str(), X_OK) == 0) return cmd;
        return std::nullopt;
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env) return std::nullopt;
    std::string s(path_env);
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(':', i);
        std::string dir = (j == std::string::npos) ? s.substr(i) : s.substr(i, j - i);
        i = (j == std::string::npos) ? s.size() : j + 1;
        if (dir.empty()) continue;
        std::string candidate = dir + "/" + cmd;
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return std::nullopt;
}

namespace {

std::vector<char*> make_argv(const std::vector<std::string>& argv) {
    std::vector<char*> v;
    v.reserve(argv.size() + 1);
    for (auto& a : argv) v.push_back(const_cast<char*>(a.c_str()));
    v.push_back(nullptr);
    return v;
}

SubprocessResult run_with_capture(const std::vector<std::string>& argv, bool capture) {
    SubprocessResult r;
    auto resolved = resolve_on_path(argv[0]);
    if (!resolved) {
        r.error_message = "command not found on PATH: " + argv[0];
        return r;
    }
    r.started = true;

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (capture) {
        if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
            r.error_message = std::string("pipe() failed: ") + std::strerror(errno);
            return r;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        r.error_message = std::string("fork() failed: ") + std::strerror(errno);
        if (capture) {
            for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]})
                if (fd >= 0) close(fd);
        }
        return r;
    }

    if (pid == 0) {
        if (capture) {
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(err_pipe[1], STDERR_FILENO);
            close(out_pipe[0]); close(out_pipe[1]);
            close(err_pipe[0]); close(err_pipe[1]);
        }
        std::vector<char*> av = make_argv(argv);
        execv(resolved->c_str(), av.data());
        std::string msg = "nth: exec failed for " + argv[0] + ": " + std::strerror(errno);
        ssize_t n = write(STDERR_FILENO, msg.data(), msg.size());
        (void)n;
        _exit(127);
    }

    if (capture) {
        close(out_pipe[1]);
        close(err_pipe[1]);

        auto drain = [](int fd, std::string& sink) {
            std::array<char, 4096> buf{};
            while (true) {
                ssize_t n = read(fd, buf.data(), buf.size());
                if (n > 0) sink.append(buf.data(), n);
                else if (n == 0) break;
                else if (errno == EINTR) continue;
                else break;
            }
        };
        std::vector<pollfd> pfds;
        pfds.push_back({out_pipe[0], POLLIN, 0});
        pfds.push_back({err_pipe[0], POLLIN, 0});
        while (!pfds.empty()) {
            int n = poll(pfds.data(), pfds.size(), -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            std::vector<pollfd> next;
            for (auto& p : pfds) {
                if (p.revents & (POLLIN | POLLHUP)) {
                    if (p.fd == out_pipe[0]) drain(out_pipe[0], r.stdout_text);
                    else drain(err_pipe[0], r.stderr_text);
                }
                if (!(p.revents & POLLHUP) && !(p.revents & POLLERR)) {
                    next.push_back({p.fd, POLLIN, 0});
                } else {
                    close(p.fd);
                }
            }
            pfds = std::move(next);
        }
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        r.error_message = std::string("waitpid failed: ") + std::strerror(errno);
        return r;
    }
    if (WIFEXITED(status)) {
        r.exit_code = WEXITSTATUS(status);
        r.signaled = false;
    } else if (WIFSIGNALED(status)) {
        r.signaled = true;
        r.exit_code = 128 + WTERMSIG(status);
    }
    return r;
}

} // namespace

SubprocessResult run_subprocess(const std::vector<std::string>& argv) {
    return run_with_capture(argv, /*capture=*/true);
}

SubprocessResult run_subprocess_inherit(const std::vector<std::string>& argv) {
    return run_with_capture(argv, /*capture=*/false);
}

#endif // _WIN32

} // namespace nth::util
