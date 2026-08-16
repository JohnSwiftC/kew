//
// Created by johns on 8/16/2026.
//

#include "service.h"

service::service(std::string name, std::string repo): name { std::move(name)}, repo { std::move(repo)}, init_script() {}

service::service(std::string name, std::string repo, std::string init_script): name { std::move(name) }, repo { std::move(repo) }, init_script {std::move(init_script)} {
}
