#include "framework.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace bgmedia::test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

} // namespace bgmedia::test

int main(int argc, char** argv) {
    std::string filter;
    if (argc > 1) {
        filter = argv[1];
    }

    auto& tests = bgmedia::test::registry();
    std::sort(tests.begin(), tests.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });

    int passed = 0;
    int failed = 0;
    int skipped = 0;
    const auto suiteStart = std::chrono::steady_clock::now();

    for (const auto& test : tests) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) {
            continue;
        }
        std::cout << "[ RUN      ] " << test.name << std::flush;
        const auto start = std::chrono::steady_clock::now();
        try {
            test.function();
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
            std::cout << "\r[       OK ] " << test.name << " (" << std::fixed
                      << std::setprecision(1) << ms << " ms)\n";
            ++passed;
        } catch (const bgmedia::test::Failure& failure) {
            std::cout << "\r[  FAILED  ] " << test.name << "\n            "
                      << failure.what() << '\n';
            ++failed;
        } catch (const std::exception& ex) {
            std::cout << "\r[  FAILED  ] " << test.name
                      << "\n            unexpected exception: " << ex.what() << '\n';
            ++failed;
        } catch (...) {
            std::cout << "\r[  FAILED  ] " << test.name
                      << "\n            unexpected non-standard exception\n";
            ++failed;
        }
    }

    if (passed + failed + skipped == 0) {
        std::cerr << "no tests matched filter '" << filter << "'\n";
        return 2;
    }

    const double totalMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - suiteStart)
                               .count();
    std::cout << "\n" << passed << " passed, " << failed << " failed in " << std::fixed
              << std::setprecision(1) << totalMs << " ms\n";
    return failed == 0 ? 0 : 1;
}
