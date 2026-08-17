//
// Created by johns on 8/16/2026.
//

#ifndef KEW_SERVICE_H
#define KEW_SERVICE_H
#include <string>
#include <vector>


class service {
private:
    std::string name;
    std::string repo;
public:
    service(std::string name, std::string repo);

    [[nodiscard]] bool build_service();
    [[nodiscard]] bool run_service();
};


#endif //KEW_SERVICE_H
