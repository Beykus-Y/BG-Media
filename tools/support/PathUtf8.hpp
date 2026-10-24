#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace bgmedia::test {

inline std::filesystem::path pathFromUtf8(const char* value) {
    if (!value) {
        return {};
    }
    const std::u8string text(reinterpret_cast<const char8_t*>(value));
    return std::filesystem::path(text);
}

inline std::filesystem::path pathFromUtf8(const std::string& value) {
    return pathFromUtf8(value.c_str());
}

inline std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

// Windows main() receives the command line in the active code page. Convert
// the wide command line to UTF-8 without losing non-ASCII file names.
struct Utf8CommandLine {
    int count = 0;
    char** values = nullptr;
    std::vector<std::string> storage;
    std::vector<char*> pointers;

    Utf8CommandLine(int argc, char** argv) {
#ifdef _WIN32
        int wideArgc = 0;
        LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
        if (wideArgv && wideArgc > 0) {
            storage.reserve(static_cast<std::size_t>(wideArgc));
            for (int i = 0; i < wideArgc; ++i) {
                const int length = WideCharToMultiByte(CP_UTF8, 0, wideArgv[i], -1,
                                                       nullptr, 0, nullptr, nullptr);
                std::string value;
                if (length > 1) {
                    value.resize(static_cast<std::size_t>(length), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, wideArgv[i], -1, value.data(),
                                        length, nullptr, nullptr);
                    value.resize(static_cast<std::size_t>(length - 1));
                }
                storage.push_back(std::move(value));
            }
            LocalFree(wideArgv);
            pointers.reserve(storage.size());
            for (std::string& value : storage) {
                pointers.push_back(value.data());
            }
            count = wideArgc;
            values = pointers.data();
            return;
        }
#endif
        count = argc;
        values = argv;
    }
};

} // namespace bgmedia::test
