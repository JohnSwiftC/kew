#include "service.h"
#include <iostream>

int main(int argc, char *argv[]) {

  service test_service{"test", "", "master",
                       "https://github.com/JohnSwiftC/test.git"};

  return 0;
}
