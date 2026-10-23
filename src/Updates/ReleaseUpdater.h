#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace awh::updates
{
    struct Result
    {
        bool installed = false;
        std::string message;
    };

    Result CheckAndInstall( const std::filesystem::path& gamePath, const std::string& current, const std::shared_ptr<std::atomic<bool>>& cancel );
}
