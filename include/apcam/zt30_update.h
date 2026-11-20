#pragma once
#include "zt30_trial.h"

namespace apcam_zt30 {
struct UpdateServices {
    bool (*restart)(void *, const std::string &, std::string &);
    bool (*ready)(void *, const std::string &, bool &, std::string &);
    void *context;
};
struct UpdateResult {
    std::string selected, previous;
    bool activation_attempted = false, restored = false, thermal = false;
};
inline bool link_slot(const std::string &root, const char *name, std::string &id, std::string &error)
{
    char target[128]; ssize_t n = readlink((root+"/"+name).c_str(), target, sizeof(target)-1);
    if (n < 0) return fail(error, std::string("Missing ")+name+" application selection");
    target[n] = 0;
    if (strncmp(target, "slots/", 6) || !slot_id_valid(target+6)) return fail(error, "Invalid application selection");
    id = target+6; return true;
}
// Caller holds restart lock, then slot-writer lock throughout. Restart and
// liveness are explicit dependencies, so failures can be tested without media
// hardware. No vendor fallback and no deletion of any possibly live slot.
inline bool switch_application(const std::string &root, const std::string &runtime_root,
                               const std::string &id, const UpdateServices &services,
                               UpdateResult &result, std::string &error)
{
    result.selected = id;
    Trial previous_trial;
    if (!read_trial(root, previous_trial, error)) return false;
    if (previous_trial.pending()) return fail(error, "An application trial is already pending");
    if (!services.restart || !services.ready || !link_slot(root, "active", result.previous, error) ||
        !verify_bundle_slot(root, result.previous.c_str(), runtime_root, error) ||
        !verify_bundle_slot(root, id.c_str(), runtime_root, error)) return false;
    if (id == result.previous) return fail(error, "This application bundle is already selected");
    // activate may publish before a directory fsync error. Inspect active and
    // recover explicitly; never assume a failed return means no publication.
    Trial trial;
    struct stat previous_stat{};
    if (!lstat((root+"/previous").c_str(), &previous_stat)) {
        if (!link_slot(root, "previous", trial.rollback_previous, error)) return false;
    } else if (errno != ENOENT) return fail(error, "Cannot inspect previous application selection");
    trial.rollback_previous_saved = true;
    trial.previous = result.previous; trial.candidate = id; trial.boot = boot_identity();
    trial.state = "starting"; trial.deadline = trial_now()+180000;
    if (!token(trial.boot, 80) || !write_trial(root, trial, error)) return false;
    result.activation_attempted = true;
    bool restarted = false;
    // The journal must precede either selection mutation so recovery retains
    // the older manual-restore target even after a crash before activation.
    bool ok = replace_link(root, "previous", "slots/"+result.previous, error) &&
              activate_bundle(root, id.c_str(), runtime_root, error);
    if (ok) {
        restarted = true;
        ok = services.restart(services.context, id, error) && services.ready(services.context, id, result.thermal, error);
    }
    if (ok) {
        trial.state = "ready"; trial.deadline = trial_now()+60000;
        if (write_trial(root, trial, error)) return true;
    }
    std::string cause = error, recovery_error, current;
    if (!link_slot(root, "active", current, recovery_error)) {
        error = cause+"; active selection unreadable: "+recovery_error; return false;
    }
    if (current != id && current != result.previous) {
        trial.state = "abandoned"; (void)write_trial(root, trial, recovery_error);
        error = cause+"; application selection changed outside the transaction"; return false;
    }
    trial.state = "recovering";
    if (!write_trial(root, trial, recovery_error)) {
        error = cause+"; rollback journal failed: "+recovery_error; return false;
    }
    if (current != result.previous &&
        !activate_bundle(root, result.previous.c_str(), runtime_root, recovery_error)) {
        error = cause+"; AP rollback selection failed: "+recovery_error; return false;
    }
    bool thermal = false;
    if ((restarted && !services.restart(services.context, result.previous, recovery_error)) ||
        !services.ready(services.context, result.previous, thermal, recovery_error)) {
        error = cause+"; previous AP application did not recover: "+recovery_error; return false;
    }
    if (!restore_trial_previous(root, trial, recovery_error)) {
        error = cause+"; AP application recovered but previous selection recovery failed: "+recovery_error; return false;
    }
    trial.state = "rolled-back";
    if (!write_trial(root, trial, recovery_error)) {
        error = cause+"; AP application recovered but trial status could not be saved: "+recovery_error; return false;
    }
    result.restored = true; result.thermal = thermal;
    error = cause+"; previous AP application restored"; return false;
}
inline bool update_application(const std::string &root, const std::string &bundle,
                               const std::string &runtime_root, const UpdateServices &services,
                               UpdateResult &result, std::string &error)
{
    std::string id;
    return stage_bundle(root, bundle, runtime_root, id, error) &&
        switch_application(root, runtime_root, id, services, result, error);
}
} // namespace apcam_zt30
