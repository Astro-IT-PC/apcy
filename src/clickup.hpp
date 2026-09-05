// ClickUp API v2 client and the "what is relevant to me" query.
#pragma once

#include <atomic>
#include <exception>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "http.hpp"

namespace clickup {

struct User {
    std::string id;
    std::string username;
    std::string email;
};

struct Team {
    std::string id;
    std::string name;
};

// One entry of a list's status workflow.
struct StatusOption {
    std::string status;
    std::string color;  // "#rrggbb"
    std::string type;   // "open" | "custom" | "closed" | "done"
    int orderindex = 0;
};

// Why a task is shown. A task can match more than one reason.
enum Source : unsigned {
    SRC_ASSIGNED  = 1u << 0,  // I am an assignee
    SRC_MENTIONED = 1u << 1,  // a comment @mentions me (or matches a mention pattern)
    SRC_COMMENTED = 1u << 2,  // I authored a comment on the task
};

struct Task {
    std::string id;
    std::string customId;
    std::string name;
    std::string url;
    std::string status;
    std::string statusColor;  // "#rrggbb"
    std::string priority;     // "urgent" | "high" | "normal" | "low" | ""
    std::string listId;
    std::string listName;
    std::string folderName;
    std::string spaceName;
    std::string teamName;
    std::string description;      // plain text, filled by Client::taskDetail
    std::vector<Task> subtasks;   // filled by Client::taskDetail
    bool detailLoaded = false;
    long long dueDateMs = 0;      // 0 = no due date
    long long dateUpdatedMs = 0;
    unsigned sources = 0;         // bitmask of Source
    std::string mentionSnippet;   // the comment text that matched, trimmed
};

enum class SortMode { Due = 0, Priority, Updated, Status, Count };

// 0 = urgent ... 3 = low, 4 = none. Used for SortMode::Priority.
int priorityRank(const std::string &priority);
void sortTasks(std::vector<Task> &tasks, SortMode mode);

struct FetchOptions {
    std::string teamId;               // empty = every workspace the token can see
    bool includeClosed = false;
    bool includeSubtasks = true;
    int commentScanDays = 14;         // 0 disables comment scanning
    int maxCommentScan = 150;         // max tasks whose comments are fetched per workspace
    std::vector<std::string> mentionPatterns;  // may contain {username} and {email}
};

struct Cancelled : std::exception {
    const char *what() const noexcept override { return "cancelled"; }
};

class Client {
public:
    Client(std::string token, const std::atomic<bool> *cancel = nullptr);

    User me();
    std::vector<Team> teams();

    // GET /team/{teamId}/task with the given (already-encoded key, raw value)
    // query parameters. Walks pages until last_page or maxTasks is reached.
    std::vector<Task> teamTasks(const std::string &teamId,
                                const std::vector<std::pair<std::string, std::string>> &params,
                                int maxTasks = -1);

    nlohmann::json comments(const std::string &taskId);

    // GET /task/{id}?include_subtasks=true: full task including its plain-text
    // description and direct subtasks.
    Task taskDetail(const std::string &taskId);
    // GET /list/{id}: the statuses a task in that list can take.
    std::vector<StatusOption> listStatuses(const std::string &listId);
    // PUT /task/{id} {"status": ...}: returns the updated task.
    Task setStatus(const std::string &taskId, const std::string &status);

private:
    nlohmann::json requestJson(const std::string &method, const std::string &pathAndQuery,
                               const std::string &body = "");
    nlohmann::json getJson(const std::string &pathAndQuery) { return requestJson("GET", pathAndQuery); }
    void checkCancel() const;

    std::string token_;
    const std::atomic<bool> *cancel_;
    HttpClient http_;
};

using ProgressFn = std::function<void(const std::string &)>;

// Collects tasks assigned to `me` plus tasks that have a comment written by me
// or mentioning me, across the selected workspaces. Sorted with SortMode::Due.
std::vector<Task> fetchMyTasks(Client &client, const User &me, const FetchOptions &opt,
                               const ProgressFn &progress);

}  // namespace clickup
