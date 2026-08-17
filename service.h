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

struct service_result {
    std::string message;
    bool success { false };
    service_state to_state { service_state::UNBUILT };
    int exit_code { 0 };
};

class service {
private:
    std::string name_;
    std::string repo_;
    service_state state_;

    // Builds a result and commits to_state to this->state_.
    service_result commit(std::string message, bool success, service_state to_state, int exit_code);
    // Result that leaves state_ untouched.
    [[nodiscard]] service_result fail(std::string message, int exit_code) const;

    // docker must be on PATH and the daemon must answer. Fills err on failure.
    [[nodiscard]] static bool docker_ready(std::string& err);
    [[nodiscard]] bool container_running() const;
    [[nodiscard]] bool container_exists() const;
    [[nodiscard]] bool image_exists() const;

public:
    service(std::string name, std::string repo);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& repo() const noexcept { return repo_; }
    [[nodiscard]] service_state state() const noexcept { return state_; }

    // Docker image names must be lowercase; container names share the charset.
    [[nodiscard]] static bool is_valid_name(const std::string& name) noexcept;
    // Rejects anything the shell would reinterpret inside double quotes.
    [[nodiscard]] static bool is_safe_path(const std::string& path) noexcept;

    [[nodiscard]] service_result build_service();
    [[nodiscard]] service_result run_service();
    [[nodiscard]] service_result kill_service();

    // Reconciles state_ with what docker actually reports.
    service_result refresh_state();
};

[[nodiscard]] const char* to_string(service_state state) noexcept;

#endif //KEW_SERVICE_H
