// TEST(name), CHECK/CHECK_EQ, TEST_MAIN() at the bottom for a main().
// registry is a function-local static so init order across TUs doesn't matter
#pragma once

#include <cstdio>
#include <sstream>
#include <vector>

namespace gredis::test {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failure_count() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) {
        registry().push_back({name, fn});
    }
};

inline int harness_main() {
    const int total = static_cast<int>(registry().size());
    int failed_tests = 0;
    for (const auto& tc : registry()) {
        const int before = failure_count();
        std::fprintf(stderr, "[ RUN      ] %s\n", tc.name);
        tc.fn();
        if (failure_count() != before) {
            ++failed_tests;
            std::fprintf(stderr, "[  FAILED  ] %s\n", tc.name);
        } else {
            std::fprintf(stderr, "[       OK ] %s\n", tc.name);
        }
    }
    std::fprintf(stderr, "[==========] %d test(s) run, %d failed.\n", total, failed_tests);
    return failed_tests == 0 ? 0 : 1;
}

} // namespace gredis::test

#define TEST(name)                                                                 \
    static void gredis_test_##name();                                             \
    static ::gredis::test::Registrar gredis_registrar_##name(#name, gredis_test_##name); \
    static void gredis_test_##name()

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "  CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ++::gredis::test::failure_count();                               \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        auto&& gredis_lhs = (a);                                             \
        auto&& gredis_rhs = (b);                                             \
        if (!(gredis_lhs == gredis_rhs)) {                                   \
            std::ostringstream gredis_lhs_os;                                \
            gredis_lhs_os << gredis_lhs;                                     \
            std::ostringstream gredis_rhs_os;                                \
            gredis_rhs_os << gredis_rhs;                                     \
            std::fprintf(stderr, "  CHECK_EQ failed: %s (%s) != %s (%s) (%s:%d)\n", \
                         #a, gredis_lhs_os.str().c_str(), #b, gredis_rhs_os.str().c_str(), \
                         __FILE__, __LINE__);                                \
            ++::gredis::test::failure_count();                               \
        }                                                                    \
    } while (0)

#define TEST_MAIN() \
    int main() { return ::gredis::test::harness_main(); }
