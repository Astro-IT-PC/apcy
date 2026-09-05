#include "config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

bool readFile(const std::string &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::string envOr(const char *name) {
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

}  // namespace

std::string userConfigDir() {
#ifdef _WIN32
    std::string base = envOr("APPDATA");
    if (base.empty()) base = envOr("USERPROFILE");
    return base.empty() ? std::string() : base + "\\apcy\\";
#else
    std::string base = envOr("XDG_CONFIG_HOME");
    if (base.empty()) {
        std::string home = envOr("HOME");
        if (home.empty()) return {};
        base = home + "/.config";
    }
    return base + "/apcy/";
#endif
}

Config loadConfig(int argc, char **argv, const std::string &appDir) {
    Config cfg;

    std::vector<std::string> candidates;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--config") candidates.push_back(argv[i + 1]);
    }
    if (std::string env = envOr("APCY_CONFIG"); !env.empty()) candidates.push_back(env);
    candidates.push_back("config.json");
    if (!appDir.empty()) candidates.push_back(appDir + "config.json");
    const std::string userDir = userConfigDir();
    if (!userDir.empty()) candidates.push_back(userDir + "config.json");

    std::string text;
    for (const auto &path : candidates) {
        if (readFile(path, text)) {
            cfg.loadedFrom = path;
            break;
        }
    }

    if (!cfg.loadedFrom.empty()) {
        json j = json::parse(text, nullptr, false, true);
        if (j.is_discarded() || !j.is_object()) {
            cfg.error = "config.json is not valid JSON (" + cfg.loadedFrom + ")";
        } else {
            cfg.token = j.value("token", cfg.token);
            cfg.teamId = j.value("team_id", cfg.teamId);
            cfg.userEmail = j.value("user_email", cfg.userEmail);
            cfg.userName = j.value("username", cfg.userName);
            cfg.includeClosed = j.value("include_closed", cfg.includeClosed);
            cfg.commentScanDays = j.value("comment_scan_days", cfg.commentScanDays);
            cfg.maxCommentScan = j.value("max_comment_scan", cfg.maxCommentScan);
            cfg.refreshMinutes = j.value("refresh_minutes", cfg.refreshMinutes);
            if (j.contains("mention_patterns") && j["mention_patterns"].is_array()) {
                cfg.mentionPatterns.clear();
                for (const auto &p : j["mention_patterns"])
                    if (p.is_string()) cfg.mentionPatterns.push_back(p.get<std::string>());
            }
        }
    }

    if (std::string env = envOr("CLICKUP_API_TOKEN"); !env.empty()) cfg.token = env;

    if (cfg.error.empty() && cfg.token.empty()) {
        cfg.error = cfg.loadedFrom.empty()
                        ? "No config.json found. Copy config.example.json to config.json next to the executable or to " +
                              (userDir.empty() ? std::string("your config directory") : userDir + "config.json") +
                              " and add your ClickUp token."
                        : "config.json has no \"token\" (" + cfg.loadedFrom + ")";
    }
    return cfg;
}

bool saveSettings(Config &cfg, const std::string &token, const std::string &email, const std::string &username,
                  std::string &error) {
    std::string path = cfg.loadedFrom;
    json j = json::object();

    if (!path.empty()) {
        std::string text;
        if (readFile(path, text)) {
            json existing = json::parse(text, nullptr, false, true);
            if (existing.is_object()) j = existing;
        }
    } else {
        const std::string dir = userConfigDir();
        if (dir.empty()) {
            error = "Could not determine a config directory (HOME / APPDATA not set)";
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        path = dir + "config.json";
    }

    // fresh file: write the full set of settings so it is easy to edit later
    if (!j.contains("team_id")) j["team_id"] = cfg.teamId;
    if (!j.contains("include_closed")) j["include_closed"] = cfg.includeClosed;
    if (!j.contains("comment_scan_days")) j["comment_scan_days"] = cfg.commentScanDays;
    if (!j.contains("max_comment_scan")) j["max_comment_scan"] = cfg.maxCommentScan;
    if (!j.contains("refresh_minutes")) j["refresh_minutes"] = cfg.refreshMinutes;
    if (!j.contains("mention_patterns")) j["mention_patterns"] = cfg.mentionPatterns;
    j["token"] = token;
    j["user_email"] = email;
    j["username"] = username;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Could not write " + path;
        return false;
    }
    out << j.dump(2) << "\n";
    if (!out) {
        error = "Could not write " + path;
        return false;
    }

    cfg.token = token;
    cfg.userEmail = email;
    cfg.userName = username;
    cfg.loadedFrom = path;
    cfg.error.clear();
    return true;
}
