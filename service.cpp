//
// Created by johns on 8/16/2026.
//

#include "service.h"

Service::Service(std::string name, std::string repo): name { std::move(name) }, repo { std::move(repo) } {}
Service::Service(std::string name, std::string repo, std::string init) : Service(std::move(name), std::move(repo)), init { std::move(init) } {}
