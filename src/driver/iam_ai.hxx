#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace kex::driver {

class IamAi {
public:
    IamAi(std::string entry, std::string outputDir);
    ~IamAi();
    IamAi(const IamAi&) = delete;
    auto operator=(const IamAi&) -> IamAi& = delete;

    // The build is over, with this exit status.
    auto finish(int status) -> void;

private:
    std::string m_entry;
    std::string m_outputDir;
    std::uint64_t m_seed;
    std::chrono::steady_clock::time_point m_start;
    std::filesystem::file_time_type m_fileStart;
    bool m_finished = false;

    auto pick(std::size_t count) -> std::size_t;
};

} // namespace kex::driver
