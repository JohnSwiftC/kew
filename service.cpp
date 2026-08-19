//
// Created by johns on 8/16/2026.
//

#include "service.h"

#include <cstdio>
#include <utility>
#include <sys/stat.h>

#ifdef _WIN32
#ifdef __STRICT_ANSI__
// MinGW hides _popen/_pclose in strict ISO mode.
extern "C" FILE* _popen(const char* command, const char* mode);
extern "C" int _pclose(FILE* stream);
#endif
#else
#include <sys/wait.h>
#endif

namespace {

// Runs cmd through the shell, capturing stdout+stderr into out.
// Returns the process exit code, or -1 if the shell could not be started.
int run_command(const std::string& cmd, std::string* out) {
    const std::string full = cmd + " 2>&1";

#ifdef _WIN32
    FILE* pipe = _popen(full.c_str(), "r");
#else
    FILE* pipe = popen(full.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return -1;
    }

    char buf[512];
    while (fgets(buf, sizeof buf, pipe) != nullptr) {
        if (out != nullptr) {
            out->append(buf);
        }
    }

#ifdef _WIN32
    return _pclose(pipe);
#else
    const int status = pclose(pipe);
    if (status == -1) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
#endif
}

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string last_line(const std::string& s) {
    const std::string t = trim(s);
    if (t.empty()) {
        return "(no output)";
    }
    const auto nl = t.find_last_of('\n');
    return nl == std::string::npos ? t : trim(t.substr(nl + 1));
}

std::string quote(const std::string& s) {
    return "\"" + s + "\"";
}

enum class path_kind { MISSING, DIRECTORY, OTHER };

// _stat on Windows rejects a trailing separator on anything but a drive root.
std::string strip_trailing_sep(const std::string& p) {
    std::string s = p;
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) {
        if (s.size() == 3 && s[1] == ':') {
            break;
        }
        s.pop_back();
    }
    return s;
}

// stat() rather than <filesystem>: GCC 6.3 (the MinGW here) has no <filesystem>,
// and <experimental/filesystem> needs -lstdc++fs. This works on every target.
path_kind classify_path(const std::string& path) {
    const std::string p = strip_trailing_sep(path);
#ifdef _WIN32
    struct _stat st;
    if (_stat(p.c_str(), &st) != 0) {
        return path_kind::MISSING;
    }
    return (st.st_mode & _S_IFDIR) ? path_kind::DIRECTORY : path_kind::OTHER;
#else
    struct stat st;
    if (stat(p.c_str(), &st) != 0) {
        return path_kind::MISSING;
    }
    return S_ISDIR(st.st_mode) ? path_kind::DIRECTORY : path_kind::OTHER;
#endif
}

}

const char* to_string(const service_state state) noexcept {
    switch (state) {
        case service_state::UNBUILT: return "UNBUILT";
        case service_state::BUILT:   return "BUILT";
        case service_state::RUNNING: return "RUNNING";
        case service_state::STOPPED: return "STOPPED";
    }
    return "UNKNOWN";
}

const char* to_string(const repo_state state) noexcept {
    switch (state) {
        case repo_state::UNKNOWN: return "UNKNOWN";
        case repo_state::NONE:    return "NONE";
        case repo_state::EXISTS:  return "EXISTS";
        case repo_state::FOREIGN: return "FOREIGN";
    }
    return "UNKNOWN";
}

service::service(std::string name, std::string repo, std::string repo_branch, std::string remote)
    : name_ { std::move(name) },
      repo_ { std::move(repo) },
      repo_branch_ { std::move(repo_branch) },
      remote_ { std::move(remote) },
      state_ { service_state::UNBUILT },
      // UNKNOWN, not NONE: nothing has looked at the disk yet.
      repo_state_ { repo_state::UNKNOWN },
      stale_ { false } {
    if (repo_.empty()) {
        repo_ = "./" + name_;
    }
    if (repo_branch_.empty()) {
        repo_branch_ = "main";
    }
}

bool service::is_valid_name(const std::string& name) noexcept {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    const char head = name.front();
    if (!((head >= 'a' && head <= 'z') || (head >= '0' && head <= '9'))) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                     || c == '_' || c == '.' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool service::is_safe_path(const std::string& path) noexcept {
    if (path.empty() || path.size() > 4096) {
        return false;
    }
    for (const unsigned char c : path) {
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
        if (c == '"' || c == '$' || c == '`' || c == '%') {
            return false;
        }
    }
    return true;
}

bool service::is_safe_url(const std::string& url) noexcept {
    if (url.empty() || url.size() > 2048) {
        return false;
    }
    // A URL starting with '-' is parsed by git as an option: "--upload-pack=..."
    // is remote code execution, not a clone.
    if (url.front() == '-') {
        return false;
    }
    for (const unsigned char c : url) {
        if (c < 0x20 || c == 0x7f || c == ' ') {
            return false;
        }
        if (c == '"' || c == '$' || c == '`' || c == '%' || c == 39) {
            return false;
        }
    }

    // Whitelist, not blacklist. git's ext:: and fd:: transports run arbitrary
    // commands by design, so anything unrecognised is refused.
    static const char* const schemes[] = {
        "https://", "http://", "ssh://", "git://", "file://"
    };
    for (const char* scheme : schemes) {
        if (url.compare(0, std::string(scheme).size(), scheme) == 0) {
            return true;
        }
    }

    // scp-like form: user@host:path
    const std::string::size_type at = url.find('@');
    const std::string::size_type colon = url.find(':');
    if (at != std::string::npos && colon != std::string::npos && at < colon && at > 0) {
        return url.find("::") == std::string::npos;
    }
    return false;
}

bool service::is_valid_branch(const std::string& branch) noexcept {
    if (branch.empty() || branch.size() > 255) {
        return false;
    }
    if (branch.front() == '-' || branch.front() == '.' || branch.front() == '/') {
        return false;
    }
    if (branch.back() == '/' || branch.back() == '.') {
        return false;
    }
    if (branch.find("..") != std::string::npos || branch.find("@{") != std::string::npos) {
        return false;
    }
    if (branch.size() >= 5 && branch.compare(branch.size() - 5, 5, ".lock") == 0) {
        return false;
    }
    for (const unsigned char c : branch) {
        if (c < 0x20 || c == 0x7f || c == ' ') {
            return false;
        }
        // git check-ref-format's rejected set, plus shell metacharacters.
        if (c == '~' || c == '^' || c == ':' || c == '?' || c == '*' || c == '[' ||
            c == 92 || c == '"' || c == '$' || c == '`' || c == '%' || c == 39) {
            return false;
        }
    }
    return true;
}

service_result service::commit(std::string message, const bool success,
                               const service_state to_state, const int exit_code) {
    state_ = to_state;
    return service_result { std::move(message), success, to_state, exit_code };
}

service_result service::fail(std::string message, const int exit_code) const {
    return service_result { std::move(message), false, state_, exit_code };
}

service_result service::commit_repo(std::string message, const bool success,
                                    const repo_state to_repo_state, const int exit_code) {
    repo_state_ = to_repo_state;
    return service_result { std::move(message), success, state_, exit_code, to_repo_state };
}

service_result service::fail_repo(std::string message, const int exit_code) const {
    return service_result { std::move(message), false, state_, exit_code, repo_state_ };
}

bool service::git_ready(std::string& err) {
    std::string out;
    if (run_command("git --version", &out) != 0) {
        err = "git unavailable: " + last_line(out);
        return false;
    }
    return true;
}

bool service::docker_ready(std::string& err) {
    std::string out;
    const int code = run_command("docker version --format \"{{.Server.Version}}\"", &out);
    if (code != 0) {
        err = "docker unavailable: " + last_line(out);
        return false;
    }
    return true;
}

bool service::container_running() const {
    std::string out;
    const int code = run_command(
        "docker container inspect -f \"{{.State.Running}}\" " + name_, &out);
    return code == 0 && trim(out) == "true";
}

bool service::container_exists() const {
    return run_command("docker container inspect " + name_, nullptr) == 0;
}

bool service::image_exists() const {
    return run_command("docker image inspect " + name_, nullptr) == 0;
}

service_result service::build_service() {
    if (!is_valid_name(name_)) {
        return fail("Invalid service name '" + name_ +
                    "': must match [a-z0-9][a-z0-9_.-]* (docker requires lowercase).", -1);
    }
    if (!is_safe_path(repo_)) {
        return fail("Unsafe build context path '" + repo_ + "'.", -1);
    }

    // Rebuilding under a running container would leave state_ lying about which
    // image the container came from.
    if (state_ == service_state::RUNNING || container_running()) {
        return fail("Cannot build while '" + name_ + "' is running; stop it first.", -1);
    }

    std::string err;
    if (!docker_ready(err)) {
        return fail(err, -1);
    }

    std::string out;
    const std::string build_str = "docker build -t " + name_ + " " + quote(repo_);
    const int code = run_command(build_str, &out);

    if (code != 0) {
        return fail("Build failed for '" + name_ + "': " + last_line(out), code);
    }
    if (!image_exists()) {
        return fail("Build reported success but image '" + name_ + "' is missing.", -1);
    }

    // The image now matches whatever is in the work tree.
    stale_ = false;
    return commit("Built '" + name_ + "'.", true, service_state::BUILT, 0);
}

service_result service::run_service() {
    if (!is_valid_name(name_)) {
        return fail("Invalid service name '" + name_ + "'.", -1);
    }

    std::string err;
    if (!docker_ready(err)) {
        return fail(err, -1);
    }

    if (container_running()) {
        // Idempotent: asking a running service to start is not an error.
        return commit(stale_
                          ? "Service '" + name_ + "' is already running, on a stale image."
                          : "Service '" + name_ + "' is already running.",
                      true, service_state::RUNNING, 0);
    }

    if (!image_exists()) {
        stale_ = false;
        return commit("No image for '" + name_ + "'; build it first.",
                      false, service_state::UNBUILT, -1);
    }

    // The image exists but predates the current source, so starting it would
    // silently run code that is no longer in the repo.
    if (stale_) {
        return fail("Source for '" + name_ +
                    "' changed since the image was built; rebuild before running.", -1);
    }


    if (container_exists()) {
        run_command("docker rm -f " + name_, nullptr);
    }

    std::string out;
    const std::string run_str =
        "docker run -d --rm --name " + name_ + " " + name_;
    const int code = run_command(run_str, &out);

    if (code != 0) {
        return fail("Failed to start '" + name_ + "': " + last_line(out), code);
    }


    if (!container_running()) {
        return fail("Container '" + name_ +
                    "' exited immediately after start; check the image entrypoint.", -1);
    }

    return commit("Started '" + name_ + "'.", true, service_state::RUNNING, 0);
}

service_result service::kill_service() {
    if (!is_valid_name(name_)) {
        return fail("Invalid service name '" + name_ + "'.", -1);
    }

    std::string err;
    if (!docker_ready(err)) {
        return fail(err, -1);
    }

    if (!container_exists()) {
        const service_state to = (image_exists() && !stale_)
                                     ? service_state::STOPPED
                                     : service_state::UNBUILT;
        return commit("Service '" + name_ + "' is not running.", true, to, 0);
    }

    std::string out;
    const int code = run_command("docker stop -t 10 " + name_, &out);

    if (code != 0) {
        return fail("Failed to stop '" + name_ + "': " + last_line(out), code);
    }
    if (container_running()) {
        return fail("Container '" + name_ + "' is still running after docker stop.", -1);
    }

    if (container_exists()) {
        run_command("docker rm -f " + name_, nullptr);
    }

    if (stale_) {
        return commit("Stopped '" + name_ + "'; image is stale, rebuild before running.",
                      true, service_state::UNBUILT, 0);
    }
    return commit("Stopped '" + name_ + "'.", true, service_state::STOPPED, 0);
}

service_result service::refresh_state() {
    std::string err;
    if (!docker_ready(err)) {
        return fail(err, -1);
    }

    if (container_running()) {
        return commit("Service '" + name_ + "' is running.",
                      true, service_state::RUNNING, 0);
    }
    if (!image_exists()) {
        stale_ = false;
        return commit("Service '" + name_ + "' has no image.",
                      true, service_state::UNBUILT, 0);
    }

    service_state to;
    if (stale_) {

        to = service_state::UNBUILT;
    } else if (state_ == service_state::UNBUILT) {
        to = service_state::BUILT;
    } else if (state_ == service_state::RUNNING) {
        to = service_state::STOPPED;
    } else {
        to = state_;
    }
    return commit(std::string("Service '") + name_ + "' is " + to_string(to) + ".",
                  true, to, 0);
}

repo_state service::probe_repo() const {
    const path_kind at_path = classify_path(repo_);
    if (at_path == path_kind::MISSING) {
        return repo_state::NONE;
    }
    if (at_path == path_kind::OTHER) {
        // A regular file sits where the checkout belongs.
        return repo_state::FOREIGN;
    }

    if (classify_path(repo_ + "/.git") == path_kind::MISSING) {
        return repo_state::FOREIGN;
    }

    std::string out;
    const int code = run_command("git -C " + quote(repo_) + " rev-parse --show-prefix", &out);
    if (code != 0 || !trim(out).empty()) {
        return repo_state::FOREIGN;
    }
    return repo_state::EXISTS;
}

service_result service::clone_repo() {
    std::string out;

    const std::string cmd = "git clone --branch " + repo_branch_ +
                            " -- " + quote(remote_) + " " + quote(repo_);
    const int code = run_command(cmd, &out);
    if (code != 0) {
        return commit_repo("Clone of '" + remote_ + "' failed: " + last_line(out),
                           false, repo_state::NONE, code);
    }
    if (probe_repo() != repo_state::EXISTS) {
        return commit_repo("Clone reported success but '" + repo_ + "' is not a repo root.",
                           false, repo_state::FOREIGN, -1);
    }
    // A checkout that was not here a moment ago cannot match an existing image.
    stale_ = true;
    if (state_ != service_state::UNBUILT) {
        state_ = service_state::UNBUILT;
    }
    return commit_repo("Cloned '" + remote_ + "' (" + repo_branch_ + ") into '" + repo_ + "'.",
                       true, repo_state::EXISTS, 0);
}

service_result service::update_repo() {
    std::string out;


    if (run_command("git -C " + quote(repo_) + " remote get-url origin", &out) != 0) {
        return commit_repo("'" + repo_ + "' has no origin remote.", false, repo_state::FOREIGN, -1);
    }
    if (trim(out) != remote_) {
        return commit_repo("'" + repo_ + "' tracks '" + trim(out) + "', not '" + remote_ + "'.",
                           false, repo_state::FOREIGN, -1);
    }

    out.clear();
    if (run_command("git -C " + quote(repo_) + " status --porcelain", &out) != 0) {
        return fail_repo("Could not read status of '" + repo_ + "': " + last_line(out), -1);
    }
    if (!trim(out).empty()) {
        return fail_repo("'" + repo_ + "' has uncommitted changes; refusing to update.", -1);
    }

    out.clear();
    if (run_command("git -C " + quote(repo_) + " fetch --prune origin", &out) != 0) {
        return fail_repo("Fetch failed for '" + repo_ + "': " + last_line(out), -1);
    }

    std::string before;
    run_command("git -C " + quote(repo_) + " rev-parse HEAD", &before);

    out.clear();
    run_command("git -C " + quote(repo_) + " rev-parse --abbrev-ref HEAD", &out);
    if (trim(out) != repo_branch_) {
        std::string co;
        if (run_command("git -C " + quote(repo_) + " checkout " + repo_branch_, &co) != 0) {
            return fail_repo("Cannot switch '" + repo_ + "' to branch '" + repo_branch_ +
                             "': " + last_line(co), -1);
        }
    }


    out.clear();
    const int code = run_command(
        "git -C " + quote(repo_) + " merge --ff-only origin/" + repo_branch_, &out);
    if (code != 0) {
        return fail_repo("Cannot fast-forward '" + repo_branch_ + "' in '" + repo_ +
                         "' (diverged or rewritten): " + last_line(out), code);
    }

    std::string after;
    run_command("git -C " + quote(repo_) + " rev-parse HEAD", &after);

    if (trim(before) == trim(after)) {
        return commit_repo("'" + repo_ + "' already up to date on '" + repo_branch_ + "'.",
                           true, repo_state::EXISTS, 0);
    }


    stale_ = true;

    const std::string head = "Updated '" + repo_ + "' to " + trim(after).substr(0, 12) +
                             " (" + repo_branch_ + ").";

    if (container_running()) {

        return commit_repo(head + " Container is still running the old image; stop and rebuild.",
                           true, repo_state::EXISTS, 0);
    }

    if (state_ != service_state::UNBUILT) {
        state_ = service_state::UNBUILT;
        return commit_repo(head + " Image is stale; demoted to UNBUILT, rebuild before running.",
                           true, repo_state::EXISTS, 0);
    }

    return commit_repo(head + " Build before running.", true, repo_state::EXISTS, 0);
}

service_result service::pull_repo() {
    if (remote_.empty()) {
        return fail_repo("No remote configured for '" + name_ + "'; nothing to pull from.", -1);
    }
    if (!is_safe_url(remote_)) {
        return fail_repo("Unsafe or unsupported remote URL '" + remote_ +
                         "': need https/http/ssh/git/file or user@host:path.", -1);
    }
    if (!is_valid_branch(repo_branch_)) {
        return fail_repo("Invalid branch name '" + repo_branch_ + "'.", -1);
    }
    if (!is_safe_path(repo_)) {
        return fail_repo("Unsafe local path '" + repo_ + "'.", -1);
    }

    std::string err;
    if (!git_ready(err)) {
        return fail_repo(err, -1);
    }

    const repo_state found = probe_repo();
    
    repo_state_ = found;

    switch (found) {
        case repo_state::NONE:
            return clone_repo();
        case repo_state::EXISTS:
            return update_repo();
        case repo_state::FOREIGN:
        case repo_state::UNKNOWN:
        default:
            // Never clone over or pull into something we did not put there.
            return commit_repo("'" + repo_ + "' exists but is not a clone of '" + remote_ +
                               "'; remove it or point the service elsewhere.",
                               false, repo_state::FOREIGN, -1);
    }
}


int main(int argc, char *argv[]) {

    return 0;
}
