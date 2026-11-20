#pragma once
#include "zt30_update.h"
#include "zt30_process.h"
#include <signal.h>
#include <netinet/in.h>

namespace apcam_zt30 {
inline bool proc_executable(pid_t pid, std::string &path)
{
    char proc[64], value[4096]; snprintf(proc, sizeof(proc), "/proc/%ld/exe", long(pid));
    ssize_t n = readlink(proc, value, sizeof(value)-1); if (n <= 0) return false;
    value[n] = 0; path = value;
    const std::string deleted = " (deleted)";
    if (path.size() > deleted.size() && path.compare(path.size()-deleted.size(), deleted.size(), deleted) == 0)
        path.resize(path.size()-deleted.size());
    return true;
}
inline std::vector<pid_t> managed_apps(const std::string &root)
{
    std::vector<pid_t> out; DIR *d = opendir("/proc"); if (!d) return out;
    while (auto *e = readdir(d)) {
        char *end; long value = strtol(e->d_name, &end, 10); if (*end || value <= 1) continue;
        std::string path;
        if (proc_executable(pid_t(value), path) && zt30_slot_app_pid(pid_t(value), path.c_str(), root.c_str())) {
            if (out.size() >= 32) break;
            out.push_back(pid_t(value));
        }
    }
    closedir(d); return out;
}
inline bool managed_web(pid_t pid, const std::string &root, unsigned port, std::string &executable)
{
    if (!proc_executable(pid, executable)) return false;
    std::string prefix = root+"/slots/", path;
    if (executable == root+"/zt30-web") path = executable;
    else {
        if (executable.compare(0, prefix.size(), prefix)) return false;
        std::string tail = executable.substr(prefix.size());
        if (tail.size() != 73 || tail.substr(64) != "/zt30-web" || !slot_id_valid(tail.substr(0, 64).c_str())) return false;
        path = executable;
    }
    char proc[64]; snprintf(proc, sizeof(proc), "/proc/%ld/exe", long(pid)); struct stat a{}, b{};
    if (stat(proc, &a) || lstat(path.c_str(), &b) || a.st_uid != geteuid() || !S_ISREG(b.st_mode) ||
        a.st_dev != b.st_dev || a.st_ino != b.st_ino) return false;
    snprintf(proc, sizeof(proc), "/proc/%ld/cmdline", long(pid));
    int fd = open(proc, O_RDONLY | O_CLOEXEC); if (fd < 0) return false;
    char buffer[4096]; ssize_t n = read(fd, buffer, sizeof(buffer)); close(fd); if (n <= 0) return false;
    std::vector<std::string> args; size_t at = 0;
    while (at < size_t(n)) {
        const char *end = static_cast<const char *>(memchr(buffer+at, 0, size_t(n)-at)); if (!end) return false;
        args.emplace_back(buffer+at, size_t(end-buffer)-at); at = size_t(end-buffer)+1;
    }
    return args.size() == 3 && args[1] == "-p" && args[2] == std::to_string(port) &&
        (args[0] == path || args[0] == root+"/zt30-web");
}
inline pid_t web_listener(const std::string &root, unsigned port, std::string *executable = nullptr)
{
    std::set<uint64_t> inodes; FILE *f = fopen("/proc/net/tcp", "r"); if (!f) return 0;
    char line[512]; unsigned rows = 0;
    while (fgets(line, sizeof(line), f) && ++rows <= 4096) {
        unsigned local_port = 0, state = 0; unsigned long long inode = 0;
        if (sscanf(line, "%*u: %*8[0-9A-Fa-f]:%x %*s %x %*s %*s %*s %*u %*u %llu", &local_port, &state, &inode) == 3 &&
            local_port == port && state == 0x0a && inodes.size() < 64) inodes.insert(inode);
    }
    fclose(f); if (inodes.empty()) return 0;
    DIR *procs = opendir("/proc"); if (!procs) return 0; pid_t result = 0;
    while (auto *e = readdir(procs)) {
        char *end; long value = strtol(e->d_name, &end, 10); if (*end || value <= 1) continue;
        std::string path; pid_t pid = pid_t(value); if (!managed_web(pid, root, port, path)) continue;
        char proc[64]; snprintf(proc, sizeof(proc), "/proc/%ld/fd", value); DIR *fds = opendir(proc); if (!fds) continue;
        unsigned count = 0;
        while (auto *entry = readdir(fds)) {
            if (++count > 4096) break;
            char target[128]; std::string name = std::string(proc)+"/"+entry->d_name;
            ssize_t n = readlink(name.c_str(), target, sizeof(target)-1); if (n <= 0) continue;
            target[n] = 0; unsigned long long inode = 0; int used = 0;
            if (sscanf(target, "socket:[%llu]%n", &inode, &used) == 1 && target[used] == 0 && inodes.count(inode)) {
                result = pid; if (executable) *executable = path; break;
            }
        }
        closedir(fds); if (result) break;
    }
    closedir(procs); return result;
}
inline bool web_responds(unsigned port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); if (fd < 0) return false;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(uint16_t(port)); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int n = connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)); pollfd p{fd, POLLOUT, 0};
    bool ok = n == 0 || (errno == EINPROGRESS && poll(&p, 1, 1000) == 1);
    int error = 0; socklen_t length = sizeof(error);
    ok = ok && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && !error;
    static const char request[] = "GET /login HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ok = ok && send(fd, request, sizeof(request)-1, MSG_NOSIGNAL) == ssize_t(sizeof(request)-1);
    char header[16]{}; size_t got = 0; int64_t deadline = apcam::probe_milliseconds()+1000;
    while (ok && got < sizeof(header)-1) {
        p.events = POLLIN; int64_t left = deadline-apcam::probe_milliseconds();
        if (left <= 0 || poll(&p, 1, int(left)) != 1) {ok = false; break;}
        ssize_t bytes = recv(fd, header+got, sizeof(header)-1-got, 0);
        if (bytes < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (bytes <= 0) {ok = false; break;} got += size_t(bytes);
    }
    close(fd); return ok && !memcmp(header, "HTTP/1.1 200", 12);
}
inline bool stop_trial_apps(const std::string &root, const std::string &candidate, std::string &error)
{
    const std::string expected = root+"/slots/"+candidate+"/camera-app";
    auto candidates = [&]() {
        std::vector<pid_t> result;
        for (auto pid : managed_apps(root)) {std::string path; if (proc_executable(pid, path) && path == expected) result.push_back(pid);}
        return result;
    };
    auto apps = candidates();
    for (auto pid : apps) {
        std::string path;
        if (proc_executable(pid, path) && path == expected && zt30_slot_app_pid(pid, path.c_str(), root.c_str())) kill(pid, SIGTERM);
    }
    for (unsigned attempt = 0; attempt < 30; ++attempt) {
        if (candidates().empty()) return true;
        usleep(100000);
    }
    for (auto pid : apps) {
        std::string path;
        if (proc_executable(pid, path) && path == expected && zt30_slot_app_pid(pid, path.c_str(), root.c_str())) kill(pid, SIGKILL);
    }
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        if (candidates().empty()) return true;
        usleep(100000);
    }
    return fail(error, "Previous selection restored but camera processes did not stop");
}
// Supervisors call this under restart then writer locks. Confirm only after
// actual candidate app, live RGB and candidate LISTEN owner respond. On boot,
// deadline or three web launch failures, compare active before AP-only rollback.
inline bool maintain_trial(const std::string &root, const std::string &runtime_root, unsigned port,
                           bool launch, std::string &error, unsigned video_port = 8555)
{
    Trial t; if (!read_trial(root, t, error)) return false; if (!t.pending()) return true;
    std::string active; if (!link_slot(root, "active", active, error)) return false;
    if (active != t.candidate && !((t.state == "recovering" || t.state == "starting") && active == t.previous)) {
        t.state = "abandoned"; return write_trial(root, t, error);
    }
    std::string executable; pid_t listener = web_listener(root, port, &executable);
    std::string expected = root+"/slots/"+t.candidate;
    bool healthy = false;
    if (t.state == "ready" && executable == expected+"/zt30-web" && web_responds(port)) {
        for (auto app : managed_apps(root)) {
            std::string path;
            if (proc_executable(app, path) && path == expected+"/camera-app") {
                healthy = apcam::live_video_probe(video_port, 0, 3000); break;
            }
        }
    }
    const std::string boot = boot_identity();
    if (healthy && t.state == "ready" && !boot.empty() && t.boot == boot) {
        t.state = "confirmed"; return write_trial(root, t, error);
    }
    if (t.state == "recovering" || (t.state == "starting" && active == t.previous) || (!boot.empty() && t.boot != boot) || trial_now() >= t.deadline || (!healthy && launch && t.attempts >= 3)) {
        t.state = "recovering";
        if (!write_trial(root, t, error)) return false;
        if (active == t.candidate && !activate_bundle(root, t.previous.c_str(), runtime_root, error)) return false;
        if (!stop_trial_apps(root, t.candidate, error)) return false;
        if (listener && executable == expected+"/zt30-web") {
            std::string checked;
            if (managed_web(listener, root, port, checked) && checked == executable) kill(listener, SIGTERM);
        }
        if (!restore_trial_previous(root, t, error)) return false;
        t.state = "rolled-back"; return write_trial(root, t, error);
    }
    if (t.state == "ready" && listener && executable != expected+"/zt30-web") {
        std::string checked;
        if (managed_web(listener, root, port, checked) && checked == executable) kill(listener, SIGTERM);
    }
    if (launch && t.state == "ready") {++t.attempts; return write_trial(root, t, error);}
    return true;
}
} // namespace apcam_zt30
