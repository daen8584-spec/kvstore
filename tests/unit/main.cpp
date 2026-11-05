#include <cstring>

#include "test.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (const test::Case& c : test::registry()) {
        if (filter && std::strstr(c.name, filter) == nullptr) continue;
        ++run;
        const int before = test::failures();
        try {
            c.body();
        } catch (const test::Abort&) {
        } catch (const std::exception& e) {
            test::report(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        }
        std::cerr << (test::failures() == before ? "  ok   " : "  FAIL ") << c.name << "\n";
    }
    std::cerr << run << " tests, " << test::failures() << " failures\n";
    return test::failures() == 0 ? 0 : 1;
}
