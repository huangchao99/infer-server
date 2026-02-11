#include "infer_server/common/logger.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace infer_server {
namespace logger {

void init(const std::string& level, const std::string& log_file) {
    std::vector<spdlog::sink_ptr> sinks;

    // 控制台输出 (带颜色)
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");
    sinks.push_back(console_sink);

    // 文件输出 (可选, 5MB 轮转, 保留 3 个文件)
    if (!log_file.empty()) {
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file, 5 * 1024 * 1024, 3);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#] %v");
        sinks.push_back(file_sink);
    }

    // 创建默认 logger
    auto logger = std::make_shared<spdlog::logger>("infer_server", sinks.begin(), sinks.end());
    logger->flush_on(spdlog::level::warn);  // warn 及以上立即刷新

    spdlog::set_default_logger(logger);
    set_level(level);

    SPDLOG_INFO("Logger initialized (level={})", level);
}

void set_level(const std::string& level) {
    if (level == "trace")         spdlog::set_level(spdlog::level::trace);
    else if (level == "debug")    spdlog::set_level(spdlog::level::debug);
    else if (level == "info")     spdlog::set_level(spdlog::level::info);
    else if (level == "warn")     spdlog::set_level(spdlog::level::warn);
    else if (level == "error")    spdlog::set_level(spdlog::level::err);
    else if (level == "critical") spdlog::set_level(spdlog::level::critical);
    else                          spdlog::set_level(spdlog::level::info);
}

void shutdown() {
    spdlog::shutdown();
}

} // namespace logger
} // namespace infer_server
