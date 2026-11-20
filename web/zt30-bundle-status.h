#pragma once
static std::string zt30_bundle_version(const std::string &directory)
{
    size_t length = 0; char *data = read_file((directory+"/manifest.json").c_str(), apcam_zt30::manifest_limit, &length);
    if (!data) return T(S_UNAVAILABLE);
    std::string text(data, length); free(data);
    std::string id = directory.substr(directory.rfind('/')+1), schema;
    if (!apcam_zt30::slot_id_valid(id.c_str()) || apcam::sha256(text) != id || !apcam_zt30::manifest_identity(text, schema)) return T(S_UNAVAILABLE);
    if (schema == "apcg.zt30.slot-development.v1") return "Development ("+id.substr(0, 12)+")";
    apcam_zt30::BundleManifest m; m.text = text; std::string error;
    if (!apcam_zt30::parse_bundle_manifest(m, error)) return T(S_UNAVAILABLE);
    return m.version;
}
static std::string zt30_running_version(pid_t pid, const char *name)
{
    std::string executable;
    if (pid <= 1 || !apcam_zt30::proc_executable(pid, executable)) return T(S_UNAVAILABLE);
    std::string prefix = std::string(APP_DIR)+"/slots/", suffix = std::string("/")+name;
    if (executable.compare(0, prefix.size(), prefix) || executable.size() != prefix.size()+64+suffix.size() ||
        executable.compare(executable.size()-suffix.size(), suffix.size(), suffix)) return T(S_UNAVAILABLE);
    return zt30_bundle_version(executable.substr(0, executable.size()-suffix.size()));
}
static void append_zt30_bundle_status(APC_StringBuffer &page, pid_t camera)
{
    std::string selected, error;
    std::string selected_version = apcam_zt30::resolve(APP_DIR, selected, error) ? zt30_bundle_version(selected) : T(S_UNAVAILABLE);
    std::string app_version = zt30_running_version(camera, "camera-app"), web_version = zt30_running_version(getpid(), "zt30-web");
    const char *ids[] = {"bundle-selected", "bundle-camera", "bundle-web"};
    const enum string_id labels[] = {S_BUNDLE_SELECTED, S_BUNDLE_CAMERA, S_BUNDLE_WEB};
    const std::string versions[] = {selected_version, app_version, web_version};
    for (unsigned n=0; n<3; ++n) {
        page.appendf("<tr><th>%s</th><td id=%s>", T(labels[n]), ids[n]); page.append_html(versions[n].c_str()); page.append("</td></tr>");
    }
    apcam_zt30::Trial trial;
    if (apcam_zt30::read_trial(APP_DIR, trial, error) && !trial.state.empty()) {
        page.appendf("<tr><th>%s</th><td id=bundle-state>", T(S_BUNDLE_STATE));
        if (trial.state == "rolled-back" || trial.state == "recovering") page.append_html(T(S_APP_UPDATE_FAILED));
        else if (trial.pending()) page.append_html(T(S_APP_UPDATE_PENDING));
        else page.append_html(trial.state.c_str());
        page.append("</td></tr>");
    }
}
