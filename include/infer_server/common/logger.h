#pragma once

/**
 * @file logger.h
 * @brief 日志系统 (基于 spdlog)
 * 
 * 提供统一的日志初始化和便捷宏。
 * 日志格式: [2025-01-01 12:00:00.123] [level] [source:line] message
 */

#include <spdlog/spdlog.h>
#include <string>

namespace infer_server {
namespace logger {

/// 初始化日志系统
/// @param level 日志级别: "trace", "debug", "info", "warn", "error", "critical"
/// @param log_file 日志文件路径 (空字符串表示仅输出到控制台)
void init(const std::string& level = "info", const std::string& log_file = "");

/// 动态调整日志级别
void set_level(const std::string& level);

/// 关闭日志系统
void shutdown();

} // namespace logger
} // namespace infer_server

// ============================================================
// 便捷日志宏 (自动记录文件和行号)
// ============================================================
#define LOG_TRACE(...)    SPDLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...)    SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)     SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)
