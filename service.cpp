//
// Created by johns on 8/16/2026.
//

#include "service.h"

#include <cstdio>
#include <utility>

#ifdef _WIN32
#ifdef __STRICT_ANSI__
/
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

service::service(std::string name, std::string repo, std::string repo_branch)
    : name_ { std::move(name) },
      repo_ { std::move(repo) },
      repo_branch_ { std::move(repo_branch) },
      state_ { service_state::UNBUILT },
      repo_state_ { repo_state::NONE } {
    if (repo_.empty()) {
        repo_ = "./" + name_;
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

service_result service::commit(std::string message, const bool success,
                               const service_state to_state, const int exit_code) {
    state_ = to_state;
    return service_result { std::move(message), success, to_state, exit_code };
}

service_result service::fail(std::string message, const int exit_code) const {
    return service_result { std::move(message), false, state_, exit_code };
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
        return commit("Service '" + name_ + "' is already running.",
                      true, service_state::RUNNING, 0);
    }

    if (!image_exists()) {
        return commit("No image for '" + name_ + "'; build it first.",
                      false, service_state::UNBUILT, -1);
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
        return commit("Service '" + name_ + "' is not running.",
                      true, image_exists() ? service_state::STOPPED : service_state::UNBUILT, 0);
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
        return commit("Service '" + name_ + "' has no image.",
                      true, service_state::UNBUILT, 0);
    }
    const service_state to = state_ == service_state::UNBUILT
                                 ? service_state::BUILT
                                 : state_ == service_state::RUNNING
                                       ? service_state::STOPPED
                                       : state_;
    return commit(std::string("Service '") + name_ + "' is " + to_string(to) + ".",
                  true, to, 0);
}

bool service::pull_repo() noexcept {

}


int main(int argc, char *argv[]) {

    return 0;
}
