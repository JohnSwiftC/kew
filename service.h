//
// Created by johns on 8/16/2026.
//

#ifndef KEW_SERVICE_H
#define KEW_SERVICE_H
#include <string>


class service {
private:
    std::string name;
    std::string repo;
    std::string init_script;
public:
    service(std::string name, std::string repo);
    service(std::string name, std::string repo, std::string init_script);
};


#endif //KEW_SERVICE_H
