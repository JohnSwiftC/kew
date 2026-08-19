//
// Created by johns on 8/16/2026.
//

#ifndef KEW_SERVICE_H
#define KEW_SERVICE_H
#include <string>

enum class service_state {
    UNBUILT,
    BUILT,
    RUNNING,
    STOPPED,
};

enum class repo_state {
    UNKNOWN,  // not probed yet
    NONE,     // nothing at the local path; clone
    EXISTS,   // a clone of remote_ sits at the local path; pull
    FOREIGN,  // something else is there; never clone over or pull into it
};

struct service_result {
    std::string message;
    bool success { false };
    service_state to_state { service_state::UNBUILT };
    int exit_code { 0 };
    repo_state to_repo_state { repo_state::UNKNOWN };
};

class service {
private:
    std::string name_;
    std::string repo_;
    std::string repo_branch_;
    std::string remote_;
    service_state state_;
    repo_state repo_state_;
    // Source moved past the image. docker cannot see this, so refresh_state
    // would otherwise promote the demotion straight back to BUILT.
    bool stale_;

    // Builds a result and commits to_state to this->state_.
    service_result commit(std::string message, bool success, service_state to_state, int exit_code);
    // Result that leaves state_ untouched.
    [[nodiscard]] service_result fail(std::string message, int exit_code) const;
    // Repo equivalents: commit_repo writes repo_state_, fail_repo leaves it alone.
    service_result commit_repo(std::string message, bool success, repo_state to_repo_state, int exit_code);
    [[nodiscard]] service_result fail_repo(std::string message, int exit_code) const;

    // docker must be on PATH and the daemon must answer. Fills err on failure.
    [[nodiscard]] static bool docker_ready(std::string& err);
    [[nodiscard]] static bool git_ready(std::string& err);
    [[nodiscard]] bool container_running() const;
    [[nodiscard]] bool container_exists() const;
    [[nodiscard]] bool image_exists() const;

    [[nodiscard]] service_result clone_repo();
    [[nodiscard]] service_result update_repo();

public:
    // repo is the local checkout path and docker build context; it defaults to
    // "./" + name. remote may be left empty for a service whose source is
    // managed by hand -- only pull_repo requires it.
    service(std::string name, std::string repo, std::string repo_branch, std::string remote = {});

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& remote() const noexcept { return remote_; }
    [[nodiscard]] const std::string& branch() const noexcept { return repo_branch_; }
    [[nodiscard]] const std::string& repo() const noexcept { return repo_; }
    [[nodiscard]] service_state state() const noexcept { return state_; }
    [[nodiscard]] repo_state repo_status() const noexcept { return repo_state_; }
    // True when pull_repo moved HEAD and no build has happened since.
    [[nodiscard]] bool is_stale() const noexcept { return stale_; }

    // Docker image names must be lowercase; container names share the charset.
    [[nodiscard]] static bool is_valid_name(const std::string& name) noexcept;
    // Rejects anything the shell would reinterpret inside double quotes.
    [[nodiscard]] static bool is_safe_path(const std::string& path) noexcept;
    // Scheme whitelist: a leading '-' is argument injection and ext:: is arbitrary code.
    [[nodiscard]] static bool is_safe_url(const std::string& url) noexcept;
    [[nodiscard]] static bool is_valid_branch(const std::string& branch) noexcept;

    [[nodiscard]] service_result build_service();
    [[nodiscard]] service_result run_service();
    [[nodiscard]] service_result kill_service();

    // Clones if absent, fast-forwards if present. Refuses anything ambiguous.
    [[nodiscard]] service_result pull_repo();

    // Asks the filesystem and git what is actually at repo_.
    [[nodiscard]] repo_state probe_repo() const;

    // Reconciles state_ with what docker actually reports.
    service_result refresh_state();
};

[[nodiscard]] const char* to_string(service_state state) noexcept;
[[nodiscard]] const char* to_string(repo_state state) noexcept;

#endif //KEW_SERVICE_H
