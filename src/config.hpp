#pragma once

#include <string>
#include <vector>

struct Config {
    std::string token;
    std::string teamId;
    bool includeClosed = false;
    int commentScanDays = 14;
    int maxCommentScan = 150;
    int refreshMinutes = 10;
    std::vector<std::string> mentionPatterns = {"@{username}", "@author:{email}", "{email}"};

    std::string loadedFrom;  // path of the file that was read, empty if none
    std::string error;       // human readable problem, empty if fine
};

// Per-user directory for config.json / window.json, with a trailing separator:
// %APPDATA%\apcy\ on Windows, $XDG_CONFIG_HOME/apcy/ or ~/.config/apcy/ elsewhere.
// Empty if it cannot be determined.
std::string userConfigDir();

// Resolution order: --config <path>, $APCY_CONFIG, ./config.json,
// <executable dir>/config.json, <userConfigDir>/config.json.
// $CLICKUP_API_TOKEN overrides the token.
Config loadConfig(int argc, char **argv, const std::string &appDir);
