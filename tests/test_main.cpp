#include <iostream>

#include "test_framework.hpp"

int main() {
    for (auto& t : testing::registry()) {
        std::cout << "RUN  " << t.name << "\n";
        t.fn();
    }
    if (testing::failures > 0) {
        std::cerr << testing::failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all tests passed (" << testing::registry().size() << " suites)\n";
    return 0;
}
