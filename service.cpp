//
// Created by johns on 8/16/2026.
//

#include "service.h"

#include <vector>


service::service(std::string name, std::string repo): name { std::move(name)}, repo { std::move(repo)} {}

//docker build -t myproj ./myproj
//docker run -d --rm --name job-xyz myproj

bool service::build_service() {
    const std::string build_str = "docker build -t " + name + " ./" + name;
    return !system(build_str.c_str());
}

bool service::run_service() {
    const std::string run_str = "docker run -d --rm --name " + name + " " + name;
    return !system(run_str.c_str());
}



int main(int argc, char *argv[]) {

    return 0;
}

