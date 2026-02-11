#include "infer_server/common/config.h"
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

namespace infer_server {

void ConfigManager::ensure_directory(const std::string& file_path) {
    fs::path p(file_path);
    fs::path dir = p.parent_path();
    if (!dir.empty() && !fs::exists(dir)) {
        fs::create_directories(dir);
    }
}

ServerConfig ConfigManager::load_server_config(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw std::runtime_error("Cannot open server config file: " + path);
    }
    nlohmann::json j = nlohmann::json::parse(ifs);
    return j.get<ServerConfig>();
}

void ConfigManager::save_server_config(const std::string& path, const ServerConfig& config) {
    ensure_directory(path);
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        throw std::runtime_error("Cannot write server config file: " + path);
    }
    nlohmann::json j = config;
    ofs << j.dump(2) << std::endl;
}

std::vector<StreamConfig> ConfigManager::load_streams(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw std::runtime_error("Cannot open streams config file: " + path);
    }
    nlohmann::json j = nlohmann::json::parse(ifs);
    return j.at("streams").get<std::vector<StreamConfig>>();
}

void ConfigManager::save_streams(const std::string& path, const std::vector<StreamConfig>& streams) {
    ensure_directory(path);
    nlohmann::json j;
    j["streams"] = streams;
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        throw std::runtime_error("Cannot write streams config file: " + path);
    }
    ofs << j.dump(2) << std::endl;
}

} // namespace infer_server
