#include "clickup.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <unordered_map>

using nlohmann::json;

namespace clickup {

namespace {

const char *kBaseUrl = "https://api.clickup.com/api/v2";

// ClickUp mixes numbers and strings for ids; normalise everything to string.
std::string jstr(const json &j, const char *key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    if (it == j.end()) return {};
    const json &v = *it;
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) return std::to_string((long long)v.get<double>());
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return {};
}

long long jms(const json &j, const char *key) {
    std::string s = jstr(j, key);
    return s.empty() ? 0 : std::strtoll(s.c_str(), nullptr, 10);
}

const json &jobj(const json &j, const char *key) {
    static const json empty = json::object();
    if (!j.is_object()) return empty;
    auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return empty;
    return *it;
}

std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string replaceAll(std::string s, const std::string &from, const std::string &to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string trimSnippet(const std::string &text, size_t maxLen = 120) {
    std::string out;
    out.reserve(text.size());
    bool lastSpace = false;
    for (unsigned char c : text) {
        bool sp = std::isspace(c) != 0;
        if (sp && lastSpace) continue;
        out.push_back(sp ? ' ' : (char)c);
        lastSpace = sp;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out.size() > maxLen) {
        out.resize(maxLen);
        // do not cut a UTF-8 sequence in half
        while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80) out.pop_back();
        out += "...";
    }
    return out;
}

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Task parseTask(const json &t, const std::string &teamName) {
    Task task;
    task.id = jstr(t, "id");
    task.customId = jstr(t, "custom_id");
    task.name = jstr(t, "name");
    task.url = jstr(t, "url");
    task.status = jstr(jobj(t, "status"), "status");
    task.statusColor = jstr(jobj(t, "status"), "color");
    task.priority = jstr(jobj(t, "priority"), "priority");
    task.listId = jstr(jobj(t, "list"), "id");
    task.listName = jstr(jobj(t, "list"), "name");
    task.folderName = jstr(jobj(t, "folder"), "name");
    task.spaceName = jstr(jobj(t, "space"), "name");
    task.teamName = teamName;
    task.dueDateMs = jms(t, "due_date");
    task.dateUpdatedMs = jms(t, "date_updated");
    task.description = jstr(t, "text_content");
    if (task.description.empty()) task.description = jstr(t, "description");
    if (task.url.empty() && !task.id.empty()) task.url = "https://app.clickup.com/t/" + task.id;
    auto subs = t.find("subtasks");
    if (subs != t.end() && subs->is_array()) {
        for (const auto &s : *subs)
            if (s.is_object()) task.subtasks.push_back(parseTask(s, teamName));
    }
    return task;
}

bool dueLess(const Task &a, const Task &b) {
    bool ad = a.dueDateMs > 0, bd = b.dueDateMs > 0;
    if (ad != bd) return ad;  // tasks with a due date first
    if (ad && a.dueDateMs != b.dueDateMs) return a.dueDateMs < b.dueDateMs;
    return a.dateUpdatedMs > b.dateUpdatedMs;
}

}  // namespace

// ---------------------------------------------------------------------------
// Sorting
// ---------------------------------------------------------------------------

int priorityRank(const std::string &priority) {
    const std::string p = lower(priority);
    if (p == "urgent" || p == "1") return 0;
    if (p == "high" || p == "2") return 1;
    if (p == "normal" || p == "3") return 2;
    if (p == "low" || p == "4") return 3;
    return 4;
}

void sortTasks(std::vector<Task> &tasks, SortMode mode) {
    switch (mode) {
        case SortMode::Priority:
            std::stable_sort(tasks.begin(), tasks.end(), [](const Task &a, const Task &b) {
                int ra = priorityRank(a.priority), rb = priorityRank(b.priority);
                if (ra != rb) return ra < rb;
                return dueLess(a, b);
            });
            break;
        case SortMode::Updated:
            std::stable_sort(tasks.begin(), tasks.end(),
                             [](const Task &a, const Task &b) { return a.dateUpdatedMs > b.dateUpdatedMs; });
            break;
        case SortMode::Status:
            std::stable_sort(tasks.begin(), tasks.end(), [](const Task &a, const Task &b) {
                const std::string sa = lower(a.status), sb = lower(b.status);
                if (sa != sb) return sa < sb;
                return dueLess(a, b);
            });
            break;
        case SortMode::Due:
        default:
            std::stable_sort(tasks.begin(), tasks.end(), dueLess);
            break;
    }
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

Client::Client(std::string token, const std::atomic<bool> *cancel)
    : token_(std::move(token)), cancel_(cancel) {}

void Client::checkCancel() const {
    if (cancel_ && cancel_->load()) throw Cancelled{};
}

json Client::requestJson(const std::string &method, const std::string &pathAndQuery, const std::string &body) {
    const std::string url = kBaseUrl + pathAndQuery;
    std::vector<std::string> headers = {
        "Authorization: " + token_,
        "Accept: application/json",
    };
    if (!body.empty()) headers.push_back("Content-Type: application/json");

    for (int attempt = 0; attempt < 4; ++attempt) {
        checkCancel();
        HttpResponse resp = http_.request(method, url, headers, body);

        if (resp.status == 429) {
            int wait = 15;
            if (!resp.retryAfter.empty()) wait = std::max(1, std::atoi(resp.retryAfter.c_str()));
            wait = std::min(wait, 60);
            for (int i = 0; i < wait * 10; ++i) {
                checkCancel();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        if (resp.status == 401) throw std::runtime_error("ClickUp rejected the token (401). Check config.json");
        if (resp.status < 200 || resp.status >= 300) {
            std::string snippet = resp.body.substr(0, 200);
            throw std::runtime_error("ClickUp HTTP " + std::to_string(resp.status) + " for " + method + " " +
                                     pathAndQuery + ": " + snippet);
        }

        if (resp.body.empty()) return json::object();
        json j = json::parse(resp.body, nullptr, false);
        if (j.is_discarded()) throw std::runtime_error("ClickUp returned invalid JSON for " + pathAndQuery);
        return j;
    }
    throw std::runtime_error("ClickUp rate limit: gave up after several retries");
}

User Client::me() {
    json j = getJson("/user");
    const json &u = jobj(j, "user");
    User me;
    me.id = jstr(u, "id");
    me.username = jstr(u, "username");
    me.email = jstr(u, "email");
    if (me.id.empty()) throw std::runtime_error("ClickUp /user did not return a user id");
    return me;
}

std::vector<Team> Client::teams() {
    json j = getJson("/team");
    std::vector<Team> out;
    if (j.contains("teams") && j["teams"].is_array()) {
        for (const auto &t : j["teams"]) out.push_back({jstr(t, "id"), jstr(t, "name")});
    }
    return out;
}

std::vector<Task> Client::teamTasks(const std::string &teamId,
                                    const std::vector<std::pair<std::string, std::string>> &params,
                                    int maxTasks) {
    std::vector<Task> out;
    std::string teamName;
    for (const auto &kv : params)
        if (kv.first == "__team_name") teamName = kv.second;

    for (int page = 0; page < 100; ++page) {
        std::string query = "/team/" + HttpClient::urlEncode(teamId) + "/task?page=" + std::to_string(page);
        for (const auto &kv : params) {
            if (kv.first.rfind("__", 0) == 0) continue;
            query += "&" + kv.first + "=" + HttpClient::urlEncode(kv.second);
        }
        json j = getJson(query);
        if (!j.contains("tasks") || !j["tasks"].is_array() || j["tasks"].empty()) break;
        for (const auto &t : j["tasks"]) {
            out.push_back(parseTask(t, teamName));
            if (maxTasks >= 0 && (int)out.size() >= maxTasks) return out;
        }
        bool lastPage = j.value("last_page", false);
        if (lastPage) break;
    }
    return out;
}

json Client::comments(const std::string &taskId) {
    return getJson("/task/" + HttpClient::urlEncode(taskId) + "/comment");
}

Task Client::taskDetail(const std::string &taskId) {
    json j = getJson("/task/" + HttpClient::urlEncode(taskId) + "?include_subtasks=true");
    Task t = parseTask(j, "");
    t.detailLoaded = true;
    return t;
}

std::vector<StatusOption> Client::listStatuses(const std::string &listId) {
    json j = getJson("/list/" + HttpClient::urlEncode(listId));
    std::vector<StatusOption> out;
    if (j.contains("statuses") && j["statuses"].is_array()) {
        for (const auto &s : j["statuses"]) {
            StatusOption o;
            o.status = jstr(s, "status");
            o.color = jstr(s, "color");
            o.type = jstr(s, "type");
            o.orderindex = (int)jms(s, "orderindex");
            if (!o.status.empty()) out.push_back(o);
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const StatusOption &a, const StatusOption &b) { return a.orderindex < b.orderindex; });
    return out;
}

Task Client::setStatus(const std::string &taskId, const std::string &status) {
    json body = {{"status", status}};
    json j = requestJson("PUT", "/task/" + HttpClient::urlEncode(taskId), body.dump());
    return parseTask(j, "");
}

// ---------------------------------------------------------------------------
// fetchMyTasks
// ---------------------------------------------------------------------------

namespace {

struct CommentMatch {
    bool mentioned = false;
    bool commented = false;
    std::string snippet;
};

// Looks at every comment of a task. A mention is either a structured "tag"
// entry pointing at my user id or a plain-text hit of one of the patterns.
CommentMatch inspectComments(const json &payload, const User &me,
                             const std::vector<std::string> &patternsLower) {
    CommentMatch m;
    if (!payload.contains("comments") || !payload["comments"].is_array()) return m;

    for (const auto &c : payload["comments"]) {
        const std::string authorId = jstr(jobj(c, "user"), "id");
        const std::string text = jstr(c, "comment_text");
        const std::string textLower = lower(text);

        bool hit = false;
        if (c.contains("comment") && c["comment"].is_array()) {
            for (const auto &part : c["comment"]) {
                if (!part.is_object()) continue;
                const std::string partType = lower(jstr(part, "type"));
                const std::string taggedId = jstr(jobj(part, "user"), "id");
                if (!taggedId.empty() && taggedId == me.id) hit = true;
                if (partType == "tag" && taggedId == me.id) hit = true;
            }
        }
        if (!hit) {
            for (const auto &p : patternsLower) {
                if (!p.empty() && textLower.find(p) != std::string::npos) {
                    hit = true;
                    break;
                }
            }
        }

        if (hit) {
            m.mentioned = true;
            if (m.snippet.empty()) m.snippet = trimSnippet(text);
        }
        if (!authorId.empty() && authorId == me.id) {
            m.commented = true;
            if (m.snippet.empty()) m.snippet = trimSnippet(text);
        }
    }
    return m;
}

}  // namespace

std::vector<Task> fetchMyTasks(Client &client, const User &me, const FetchOptions &opt,
                               const ProgressFn &progress) {
    std::vector<Team> teams;
    if (!opt.teamId.empty()) {
        teams.push_back({opt.teamId, ""});
        // try to resolve the name; not fatal if it fails
        try {
            for (const auto &t : client.teams())
                if (t.id == opt.teamId) teams[0].name = t.name;
        } catch (const Cancelled &) {
            throw;
        } catch (...) {
        }
    } else {
        progress("Listing workspaces...");
        teams = client.teams();
    }
    if (teams.empty()) throw std::runtime_error("No ClickUp workspaces visible to this token");

    std::vector<std::string> patternsLower;
    for (const auto &p : opt.mentionPatterns) {
        std::string s = replaceAll(p, "{username}", me.username);
        s = replaceAll(s, "{email}", me.email);
        if (!s.empty() && s != "@" && s != "@author:") patternsLower.push_back(lower(s));
    }

    std::unordered_map<std::string, size_t> index;  // task id -> position in result
    std::vector<Task> result;

    auto merge = [&](Task t, unsigned src, const std::string &snippet) {
        auto it = index.find(t.id);
        if (it == index.end()) {
            t.sources = src;
            t.mentionSnippet = snippet;
            index[t.id] = result.size();
            result.push_back(std::move(t));
        } else {
            Task &existing = result[it->second];
            existing.sources |= src;
            if (existing.mentionSnippet.empty()) existing.mentionSnippet = snippet;
        }
    };

    const std::string includeClosed = opt.includeClosed ? "true" : "false";
    const std::string subtasks = opt.includeSubtasks ? "true" : "false";

    for (const auto &team : teams) {
        const std::string label = team.name.empty() ? team.id : team.name;

        // 1) tasks assigned to me
        progress("Loading tasks assigned to you (" + label + ")...");
        std::vector<Task> assigned = client.teamTasks(team.id, {
            {"__team_name", team.name},
            {"assignees%5B%5D", me.id},
            {"include_closed", includeClosed},
            {"subtasks", subtasks},
            {"order_by", "due_date"},
        });
        for (auto &t : assigned) merge(std::move(t), SRC_ASSIGNED, "");

        // 2) recently updated tasks -> inspect their comments
        if (opt.commentScanDays > 0 && opt.maxCommentScan > 0) {
            progress("Loading recently updated tasks (" + label + ")...");
            const long long since = nowMs() - (long long)opt.commentScanDays * 24LL * 3600LL * 1000LL;
            std::vector<Task> recent = client.teamTasks(team.id, {
                {"__team_name", team.name},
                {"date_updated_gt", std::to_string(since)},
                {"include_closed", includeClosed},
                {"subtasks", subtasks},
                {"order_by", "updated"},
                {"reverse", "true"},
            }, opt.maxCommentScan);

            int n = 0;
            for (auto &t : recent) {
                ++n;
                progress("Scanning comments " + std::to_string(n) + "/" + std::to_string(recent.size()) +
                         " (" + label + ")...");
                json payload = client.comments(t.id);
                CommentMatch m = inspectComments(payload, me, patternsLower);
                unsigned src = 0;
                if (m.mentioned) src |= SRC_MENTIONED;
                if (m.commented) src |= SRC_COMMENTED;
                if (src) merge(std::move(t), src, m.snippet);
            }
        }
    }

    sortTasks(result, SortMode::Due);
    return result;
}

}  // namespace clickup
