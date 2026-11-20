#include "apcam/zt30_services.h"
#include <sys/file.h>
int main(int argc, char **argv)
{
    std::string root = "/customer/camera-app", error, selected;
    int at = 1;
    std::string runtime_root = "/", run_dir = "/tmp/apcg-zt30";
    while (argc > at+2 && (!strcmp(argv[at], "--root") || !strcmp(argv[at], "--runtime-root") || !strcmp(argv[at], "--run-dir"))) {
        if (!strcmp(argv[at], "--root")) root = argv[at+1];
        else if (!strcmp(argv[at], "--run-dir")) run_dir = argv[at+1]; else runtime_root = argv[at+1];
        at += 2;
    }
    if (argc <= at || argc > at + 2) {
        fprintf(stderr, "Usage: zt30-slot [--root DIR] [--runtime-root DIR] resolve | web-listener PORT | maintain [PORT] | note-web-launch [PORT] | stage BUNDLE | activate ID | migrate ID\n"); return 2;
    }
    bool ok = false;
    if (!strcmp(argv[at], "probe")) {
        if (!apcam_zt30::canonical_root(root, error)) {fprintf(stderr, "%s\n", error.c_str()); return 1;}
        unsigned port = 80;
        if (argc == at+2) {char *end; unsigned long n = strtoul(argv[at+1], &end, 10); if (*end || !n || n > 65535) return 2; port = unsigned(n);}
        std::string executable; pid_t pid = apcam_zt30::web_listener(root, port, &executable);
        bool login = pid && apcam_zt30::web_responds(port), rgb = apcam::live_video_probe(8555, 0, 3000);
        bool thermal = rgb && apcam::live_video_probe(8555, 1, 12000);
        printf("web_pid=%ld\nweb_path=%s\nlogin=%d\nrgb=%d\nthermal=%d\n", long(pid), executable.c_str(), login, rgb, thermal);
        return pid && login && rgb ? 0 : 1;
    }
    if (!strcmp(argv[at], "web-listener") || !strcmp(argv[at], "maintain") || !strcmp(argv[at], "note-web-launch")) {
        unsigned port = 80;
        if (argc == at+2) {char *end; unsigned long n = strtoul(argv[at+1], &end, 10); if (*end || !n || n > 65535) return 2; port = unsigned(n);}
        if (!apcam_zt30::canonical_root(root, error)) {fprintf(stderr, "%s\n", error.c_str()); return 1;}
        if (!strcmp(argv[at], "web-listener")) {
            pid_t pid = apcam_zt30::web_listener(root, port); if (pid) printf("%ld\n", long(pid)); return pid ? 0 : 1;
        }
        int restart = open((run_dir+"/camera-app.restart.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (restart < 0 || flock(restart, LOCK_EX | LOCK_NB)) {if (restart >= 0) close(restart); return 3;}
        int writer = open((root+"/.slot-update.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (writer < 0 || !apcam_zt30::regular(root+"/.slot-update.lock") || flock(writer, LOCK_EX | LOCK_NB)) {
            if (writer >= 0) close(writer);
            close(restart); return 3;
        }
        ok = apcam_zt30::maintain_trial(root, runtime_root, port, !strcmp(argv[at], "note-web-launch"), error);
        close(writer); close(restart);
        if (!ok) fprintf(stderr, "Trial maintenance failed: %s\n", error.c_str());
        return ok ? 0 : 1;
    }
    if (!strcmp(argv[at], "resolve") && argc == at + 1) {
        ok = apcam_zt30::resolve(root, selected, error);
        if (ok) puts(selected.c_str());
    } else if ((!strcmp(argv[at], "activate") || !strcmp(argv[at], "migrate") || !strcmp(argv[at], "stage")) && argc == at + 2) {
        if (!apcam_zt30::canonical_root(root, error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
        int fd = open((root + "/.slot-update.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 || !apcam_zt30::regular(root + "/.slot-update.lock") || flock(fd, LOCK_EX | LOCK_NB) < 0) {
            if (fd >= 0) close(fd);
            fprintf(stderr, "Slot update is locked or unavailable\n"); return 1;
        }
        if (!strcmp(argv[at], "stage")) {
            ok = apcam_zt30::stage_bundle(root, argv[at+1], runtime_root, selected, error);
            if (ok) puts(selected.c_str());
            else if (!selected.empty()) fprintf(stderr, "Complete retained slot %s is present; publication durability unconfirmed; active unchanged\n", selected.c_str());
        } else ok = !strcmp(argv[at], "activate") ? apcam_zt30::activate_bundle(root, argv[at + 1], runtime_root, error) :
            apcam_zt30::migrate(root, argv[at + 1], error);
        close(fd);
    } else { fprintf(stderr, "Invalid slot operation\n"); return 2; }
    if (!ok) fprintf(stderr, "Slot operation failed (inspect active link if publication began): %s\n", error.c_str());
    return ok ? 0 : 1;
}
