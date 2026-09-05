// apcy - a tiny ClickUp "inbox": tasks assigned to me plus tasks where I am
// mentioned in (or wrote) a comment. UI: Clay layout + Raylib renderer.
#include "clay_bridge.h"  // raylib.h + clay.h (C linkage)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "clickup.hpp"
#include "config.hpp"
#include "textutil.hpp"

namespace {

// ---------------------------------------------------------------------------
// Palette / text styles
// ---------------------------------------------------------------------------
constexpr Clay_Color C_BG       {24, 26, 31, 255};
constexpr Clay_Color C_PANEL    {34, 37, 44, 255};
constexpr Clay_Color C_ROW      {40, 44, 52, 255};
constexpr Clay_Color C_ROW_H    {54, 59, 70, 255};
constexpr Clay_Color C_ROW_SEL  {60, 54, 96, 255};
constexpr Clay_Color C_ACCENT   {124, 92, 255, 255};
constexpr Clay_Color C_ACCENT_H {146, 118, 255, 255};
constexpr Clay_Color C_TEXT     {232, 234, 238, 255};
constexpr Clay_Color C_MUTED    {150, 156, 168, 255};
constexpr Clay_Color C_ERROR    {255, 120, 120, 255};
constexpr Clay_Color C_TAG_BG   {60, 50, 110, 255};
constexpr Clay_Color C_TAG_TEXT {210, 200, 255, 255};
constexpr Clay_Color C_SNIPPET  {190, 194, 204, 255};
constexpr Clay_Color C_CHIP     {48, 52, 62, 255};
constexpr Clay_Color C_CHIP_H   {64, 70, 84, 255};

const Clay_TextElementConfig BODY_TEXT    = {.textColor = C_TEXT, .fontId = 0, .fontSize = 17};
const Clay_TextElementConfig MUTED_TEXT   = {.textColor = C_MUTED, .fontId = 0, .fontSize = 14};
const Clay_TextElementConfig ERROR_TEXT   = {.textColor = C_ERROR, .fontId = 0, .fontSize = 14};
const Clay_TextElementConfig SNIPPET_TEXT = {.textColor = C_SNIPPET, .fontId = 0, .fontSize = 14};
const Clay_TextElementConfig TAG_TEXT     = {.textColor = C_TAG_TEXT, .fontId = 0, .fontSize = 12, .letterSpacing = 1};
const Clay_TextElementConfig BUTTON_TEXT  = {.textColor = C_TEXT, .fontId = 0, .fontSize = 15};
const Clay_TextElementConfig TAB_TEXT     = {.textColor = C_MUTED, .fontId = 0, .fontSize = 14};
const Clay_TextElementConfig TAB_ACTIVE   = {.textColor = C_TEXT, .fontId = 0, .fontSize = 14};
const Clay_TextElementConfig STATUS_TEXT  = {.textColor = C_MUTED, .fontId = 0, .fontSize = 14,
                                             .textAlignment = CLAY_TEXT_ALIGN_RIGHT};
const Clay_TextElementConfig STATUS_ERR   = {.textColor = C_ERROR, .fontId = 0, .fontSize = 14,
                                             .textAlignment = CLAY_TEXT_ALIGN_RIGHT};

// Compact variants used when the window is narrow (sidebar style).
constexpr int kCompactWidth = 620;
const Clay_TextElementConfig BODY_SM      = {.textColor = C_TEXT, .fontId = 0, .fontSize = 15};
const Clay_TextElementConfig MUTED_SM     = {.textColor = C_MUTED, .fontId = 0, .fontSize = 13};
const Clay_TextElementConfig ERROR_SM     = {.textColor = C_ERROR, .fontId = 0, .fontSize = 13};
const Clay_TextElementConfig SNIPPET_SM   = {.textColor = C_SNIPPET, .fontId = 0, .fontSize = 13};
const Clay_TextElementConfig TAG_SM       = {.textColor = C_TAG_TEXT, .fontId = 0, .fontSize = 11, .letterSpacing = 1,
                                             .wrapMode = CLAY_TEXT_WRAP_NONE};
const Clay_TextElementConfig TAB_SM       = {.textColor = C_MUTED, .fontId = 0, .fontSize = 13,
                                             .wrapMode = CLAY_TEXT_WRAP_NONE};
const Clay_TextElementConfig TAB_SM_ACT   = {.textColor = C_TEXT, .fontId = 0, .fontSize = 13,
                                             .wrapMode = CLAY_TEXT_WRAP_NONE};
const Clay_TextElementConfig BUTTON_SM    = {.textColor = C_TEXT, .fontId = 0, .fontSize = 14};

// Detail pane
const Clay_TextElementConfig DETAIL_TITLE = {.textColor = C_TEXT, .fontId = 0, .fontSize = 18};
const Clay_TextElementConfig LABEL_TEXT   = {.textColor = C_MUTED, .fontId = 0, .fontSize = 11, .letterSpacing = 2};
const Clay_TextElementConfig DESC_TEXT    = {.textColor = C_SNIPPET, .fontId = 0, .fontSize = 14, .lineHeight = 20};

// Clay does not copy string memory: every std::string passed here must stay
// alive until the frame has been rendered. All display strings live in App.
inline Clay_String S(const std::string &s) {
    return Clay_String{false, (int32_t)s.size(), s.c_str()};
}

Clay_Color parseHexColor(const std::string &hex, Clay_Color fallback) {
    if (hex.size() != 7 || hex[0] != '#') return fallback;
    unsigned v = (unsigned)std::strtoul(hex.c_str() + 1, nullptr, 16);
    return Clay_Color{(float)((v >> 16) & 255), (float)((v >> 8) & 255), (float)(v & 255), 255};
}

Clay_Color textColorOn(Clay_Color bg) {
    float lum = 0.299f * bg.r + 0.587f * bg.g + 0.114f * bg.b;
    return lum > 150.0f ? Clay_Color{20, 20, 24, 255} : Clay_Color{250, 250, 250, 255};
}

Clay_Color withAlpha(Clay_Color c, float a) { return Clay_Color{c.r, c.g, c.b, a}; }

std::string lowerAscii(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
struct Row {
    const clickup::Task *task = nullptr;
    std::string name;
    std::string meta;
    std::string metaCompact;  // shorter variant for the narrow layout
    std::string status;
    std::string tag;
    std::string snippet;
    Clay_Color pill{};
    Clay_Color pillText{};
};

// One subtask line in the detail pane.
struct SubRow {
    std::string id;
    std::string url;
    std::string name;
    std::string status;
    std::string meta;
    Clay_Color pill{};
    Clay_Color pillText{};
};

struct FetchResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    clickup::User me;
    std::vector<clickup::Task> tasks;
};

struct App;
using MainFn = std::function<void(App &)>;

// State shared with background threads.
struct Shared {
    std::mutex mu;
    std::string progress;
    std::unique_ptr<FetchResult> result;
    std::vector<MainFn> posted;  // closures to run on the UI thread
    std::atomic<bool> cancel{false};
    std::atomic<bool> busy{false};
};

struct Job {
    std::thread th;
    std::shared_ptr<std::atomic<bool>> done;
};

enum Filter { F_ALL = 0, F_ASSIGNED, F_MENTIONED, F_COMMENTED, F_COUNT };
const unsigned kFilterMask[F_COUNT] = {0xFFFFFFFFu, clickup::SRC_ASSIGNED, clickup::SRC_MENTIONED,
                                       clickup::SRC_COMMENTED};
const char *kFilterName[F_COUNT] = {"All", "Assigned", "Mentioned", "My comments"};
const char *kSortLabel[(int)clickup::SortMode::Count] = {"Sort: due", "Sort: priority", "Sort: updated",
                                                        "Sort: status"};

struct App {
    Config cfg;
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    std::thread worker;      // the big refresh
    std::vector<Job> jobs;   // small detail / status jobs

    clickup::User me;
    std::vector<clickup::Task> tasks;
    std::vector<Row> rows;
    std::vector<int> visible;
    int filter = F_ALL;
    bool filterDirty = true;
    int counts[F_COUNT] = {};
    clickup::SortMode sort = clickup::SortMode::Due;

    bool compact = false;  // narrow window -> stacked layout
    bool demo = false;
    std::string appDir;

    // per-frame display strings
    std::string tabLabels[F_COUNT];
    std::string tabLabelsShort[F_COUNT];
    std::string sortLabel;
    std::string userLine;
    std::string statusLine;
    std::string emptyLine;
    std::string errorLine;
    std::string lastUpdated;
    std::string footerLine = "Click: details   |   R: refresh   |   S: sort   |   Esc: close details   |   D: layout debugger";
    std::string footerShort = "R: refresh   |   S: sort   |   Esc: back";
    std::string openUrl;

    // detail pane
    std::string selectedId;
    std::vector<std::string> navStack;  // task ids to return to with "Back"
    std::unordered_map<std::string, clickup::Task> extraTasks;  // opened subtasks that are not in my list
    std::string detailParent;
    bool detailDirty = false;
    bool detailLoading = false;
    std::string detailError;
    std::string statusChanging;  // task id whose status is being updated
    std::unordered_map<std::string, std::vector<clickup::StatusOption>> statusesByList;
    std::string detailTitle, detailMeta, detailStatus, detailTag, detailDescription, detailNote;
    std::string subtasksLabel;
    std::vector<SubRow> subRows;
    Clay_Color detailPill{}, detailPillText{};
    std::vector<std::string> chipLabels;
    std::vector<Clay_Color> chipColors;
    std::vector<bool> chipCurrent;
    std::vector<std::vector<int>> chipRows;
    float chipAvailWidth = 0;

    double lastFetchDone = -1.0;
    double lastGeometryCheck = 0.0;
    int savedGeometry[4] = {0, 0, 0, 0};  // x, y, w, h last written to window.json
    bool debug = false;
    Font fonts[1] = {};
    Texture2D logo{};
    bool logoLoaded = false;

    // settings modal (token, email, username)
    enum { FIELD_TOKEN = 0, FIELD_EMAIL, FIELD_USERNAME, FIELD_COUNT };
    bool modalOpen = false;
    bool modalCanCancel = true;
    bool tokenVisible = false;
    int focusedField = FIELD_TOKEN;
    std::string fieldValue[FIELD_COUNT];
    std::string fieldDisplay[FIELD_COUNT];
    std::string modalError;
    std::string modalPathLine;
};

void rebuildRows(App &app);
void saveWindowState(App &app, bool force);

bool g_reinitClay = false;

void handleClayErrors(Clay_ErrorData err) {
    std::fprintf(stderr, "[clay] %.*s\n", (int)err.errorText.length, err.errorText.chars);
    if (err.errorType == CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED) {
        Clay_SetMaxElementCount(Clay_GetMaxElementCount() * 2);
        g_reinitClay = true;
    } else if (err.errorType == CLAY_ERROR_TYPE_TEXT_MEASUREMENT_CAPACITY_EXCEEDED) {
        Clay_SetMaxMeasureTextCacheWordCount(Clay_GetMaxMeasureTextCacheWordCount() * 2);
        g_reinitClay = true;
    }
}

clickup::Task *findTask(App &app, const std::string &id) {
    for (auto &t : app.tasks)
        if (t.id == id) return &t;
    auto it = app.extraTasks.find(id);
    return it == app.extraTasks.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Background work
// ---------------------------------------------------------------------------
void post(const std::shared_ptr<Shared> &shared, MainFn fn) {
    std::lock_guard<std::mutex> lock(shared->mu);
    shared->posted.push_back(std::move(fn));
}

// Runs `work` on its own thread with a fresh API client. Exceptions become a
// detail-pane error message.
void spawnJob(App &app, std::function<void(clickup::Client &)> work) {
    auto shared = app.shared;
    const std::string token = app.cfg.token;
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread th([shared, token, work, done]() {
        try {
            clickup::Client client(token, &shared->cancel);
            work(client);
        } catch (const clickup::Cancelled &) {
        } catch (const std::exception &e) {
            const std::string msg = e.what();
            post(shared, [msg](App &a) {
                a.detailError = msg;
                a.detailLoading = false;
                a.statusChanging.clear();
                a.detailDirty = true;
                std::fprintf(stderr, "[job] error: %s\n", msg.c_str());
            });
        }
        done->store(true);
    });
    app.jobs.push_back(Job{std::move(th), done});
}

void reapJobs(App &app) {
    for (size_t i = 0; i < app.jobs.size();) {
        if (app.jobs[i].done->load()) {
            app.jobs[i].th.join();
            app.jobs.erase(app.jobs.begin() + (long)i);
        } else {
            ++i;
        }
    }
}

void startFetch(App &app) {
    if (app.demo || app.cfg.token.empty() || app.shared->busy.load()) return;
    if (app.worker.joinable()) app.worker.join();  // previous run has finished

    auto shared = app.shared;
    Config cfg = app.cfg;
    shared->busy = true;
    shared->cancel = false;
    {
        std::lock_guard<std::mutex> lock(shared->mu);
        shared->progress = "Connecting to ClickUp...";
    }

    app.worker = std::thread([shared, cfg]() {
        auto res = std::make_unique<FetchResult>();
        try {
            clickup::Client client(cfg.token, &shared->cancel);
            res->me = client.me();
            // settings may override the profile of the token (mention matching, header)
            if (!cfg.userEmail.empty()) res->me.email = cfg.userEmail;
            if (!cfg.userName.empty()) res->me.username = cfg.userName;

            clickup::FetchOptions opt;
            opt.teamId = cfg.teamId;
            opt.includeClosed = cfg.includeClosed;
            opt.commentScanDays = cfg.commentScanDays;
            opt.maxCommentScan = cfg.maxCommentScan;
            opt.mentionPatterns = cfg.mentionPatterns;

            res->tasks = clickup::fetchMyTasks(client, res->me, opt, [shared](const std::string &msg) {
                std::lock_guard<std::mutex> lock(shared->mu);
                shared->progress = msg;
            });
            res->ok = true;
        } catch (const clickup::Cancelled &) {
            res->cancelled = true;
        } catch (const std::exception &e) {
            res->error = e.what();
        }
        {
            std::lock_guard<std::mutex> lock(shared->mu);
            shared->result = std::move(res);
        }
        shared->busy = false;
    });
}

// Fetches description + list statuses for the selected task.
void loadDetail(App &app, const clickup::Task &task) {
    if (app.demo) return;
    app.detailLoading = true;
    app.detailError.clear();
    auto shared = app.shared;
    const std::string id = task.id;
    const std::string listId = task.listId;
    const bool needStatuses = listId.empty() || !app.statusesByList.count(listId);

    spawnJob(app, [shared, id, listId, needStatuses](clickup::Client &c) {
        clickup::Task detail = c.taskDetail(id);
        const std::string lid = detail.listId.empty() ? listId : detail.listId;
        std::vector<clickup::StatusOption> statuses;
        if (needStatuses && !lid.empty()) statuses = c.listStatuses(lid);

        post(shared, [detail, lid, statuses, needStatuses](App &a) {
            if (clickup::Task *t = findTask(a, detail.id)) {
                t->description = detail.description;
                t->subtasks = detail.subtasks;
                t->detailLoaded = true;
                if (!detail.status.empty()) {
                    t->status = detail.status;
                    t->statusColor = detail.statusColor;
                }
                if (t->listId.empty()) t->listId = lid;
            }
            if (needStatuses && !lid.empty()) a.statusesByList[lid] = statuses;
            a.detailLoading = false;
            a.detailDirty = true;
            rebuildRows(a);
        });
    });
}

void changeStatus(App &app, const std::string &taskId, const std::string &status) {
    if (!app.statusChanging.empty()) return;
    clickup::Task *task = findTask(app, taskId);
    if (!task || lowerAscii(task->status) == lowerAscii(status)) return;

    std::string color;
    auto it = app.statusesByList.find(task->listId);
    if (it != app.statusesByList.end())
        for (const auto &o : it->second)
            if (lowerAscii(o.status) == lowerAscii(status)) color = o.color;

    app.statusChanging = taskId;
    app.detailError.clear();
    app.detailDirty = true;

    if (app.demo) {
        task->status = status;
        task->statusColor = color;
        task->dateUpdatedMs = textutil::nowMs();
        app.statusChanging.clear();
        rebuildRows(app);
        return;
    }

    auto shared = app.shared;
    spawnJob(app, [shared, taskId, status, color](clickup::Client &c) {
        clickup::Task updated = c.setStatus(taskId, status);
        post(shared, [updated, taskId, status, color](App &a) {
            if (clickup::Task *t = findTask(a, taskId)) {
                t->status = updated.status.empty() ? status : updated.status;
                t->statusColor = updated.statusColor.empty() ? color : updated.statusColor;
                if (updated.dateUpdatedMs > 0) t->dateUpdatedMs = updated.dateUpdatedMs;
            }
            a.statusChanging.clear();
            a.detailDirty = true;
            rebuildRows(a);
            std::fprintf(stderr, "[status] %s -> %s\n", taskId.c_str(), status.c_str());
        });
    });
}

void selectTask(App &app, const clickup::Task &task) {
    if (app.selectedId == task.id) return;
    app.selectedId = task.id;
    app.detailError.clear();
    app.detailDirty = true;
    const bool haveStatuses = !task.listId.empty() && app.statusesByList.count(task.listId);
    if (!task.detailLoaded || !haveStatuses) loadDetail(app, task);
}

// Click on a row of the main list: fresh navigation.
void openFromList(App &app, const clickup::Task &task) {
    app.navStack.clear();
    selectTask(app, task);
}

// Click on a subtask inside the detail pane: remember where we came from.
void openSubtask(App &app, const clickup::Task &parent, size_t index) {
    if (index >= parent.subtasks.size()) return;
    const clickup::Task &sub = parent.subtasks[index];
    if (sub.id.empty() || sub.id == app.selectedId) return;

    clickup::Task *target = findTask(app, sub.id);
    if (!target) {
        clickup::Task copy = sub;
        if (copy.listId.empty()) copy.listId = parent.listId;  // subtasks share the parent's list
        if (copy.listName.empty()) copy.listName = parent.listName;
        if (copy.spaceName.empty()) copy.spaceName = parent.spaceName;
        if (copy.folderName.empty()) copy.folderName = parent.folderName;
        copy.detailLoaded = app.demo;  // subtask entries carry no description / sub-subtasks
        target = &(app.extraTasks[copy.id] = copy);
    }
    app.navStack.push_back(app.selectedId);
    selectTask(app, *target);
}

// "Back" / Esc: return to the previous task, or close the pane.
void goBack(App &app) {
    app.detailDirty = true;
    while (!app.navStack.empty()) {
        const std::string prev = app.navStack.back();
        app.navStack.pop_back();
        if (findTask(app, prev)) {
            app.selectedId = prev;
            app.detailError.clear();
            return;
        }
    }
    app.selectedId.clear();
}

void closeDetail(App &app) {
    app.navStack.clear();
    app.selectedId.clear();
    app.detailDirty = true;
}

// ---------------------------------------------------------------------------
// Settings modal: token, and optional email / username overrides
// ---------------------------------------------------------------------------
void openSettings(App &app, const std::string &message) {
    app.modalOpen = true;
    app.modalCanCancel = !app.cfg.token.empty();
    app.fieldValue[App::FIELD_TOKEN] = app.cfg.token;
    app.fieldValue[App::FIELD_EMAIL] = app.cfg.userEmail;
    app.fieldValue[App::FIELD_USERNAME] = app.cfg.userName;
    app.focusedField = app.cfg.token.empty() ? App::FIELD_TOKEN : App::FIELD_EMAIL;
    app.tokenVisible = false;
    app.modalError = message;
}

void closeSettings(App &app) {
    app.modalOpen = false;
    app.modalError.clear();
}

// Appends printable text to the focused field. The token never contains spaces;
// usernames may.
void appendToField(App &app, const char *text) {
    if (!text) return;
    std::string &v = app.fieldValue[app.focusedField];
    const bool allowSpace = app.focusedField != App::FIELD_TOKEN;
    for (const char *p = text; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c >= 32 && c < 127 && (c != ' ' || allowSpace) && v.size() < 256) v.push_back((char)c);
    }
}

std::string trimmed(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    return s;
}

void saveSettingsFromModal(App &app) {
    const std::string token = trimmed(app.fieldValue[App::FIELD_TOKEN]);
    const std::string email = trimmed(app.fieldValue[App::FIELD_EMAIL]);
    const std::string username = trimmed(app.fieldValue[App::FIELD_USERNAME]);
    if (token.empty()) {
        app.modalError = "Paste your ClickUp API token first.";
        app.focusedField = App::FIELD_TOKEN;
        return;
    }
    if (!email.empty() && email.find('@') == std::string::npos) {
        app.modalError = "That email does not look right.";
        app.focusedField = App::FIELD_EMAIL;
        return;
    }
    std::string err;
    if (!saveSettings(app.cfg, token, email, username, err)) {
        app.modalError = err;
        return;
    }
    std::fprintf(stderr, "[config] settings saved to %s\n", app.cfg.loadedFrom.c_str());
    app.errorLine.clear();
    closeSettings(app);
    startFetch(app);
}

// Keyboard handling for the modal: typing, backspace, Ctrl/Cmd+V, Tab, Enter, Esc.
void handleModalInput(App &app) {
    const bool mod = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER) ||
                     IsKeyDown(KEY_RIGHT_SUPER);
    if (mod && IsKeyPressed(KEY_V)) appendToField(app, GetClipboardText());
    int ch = 0;
    while ((ch = GetCharPressed()) > 0) {
        if (!mod && ch >= 32 && ch < 127) {
            const char c = (char)ch;
            appendToField(app, std::string(1, c).c_str());
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        std::string &v = app.fieldValue[app.focusedField];
        if (mod) v.clear();
        else if (!v.empty()) v.pop_back();
    }
    if (IsKeyPressed(KEY_TAB)) {
        const bool back = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        app.focusedField = (app.focusedField + (back ? App::FIELD_COUNT - 1 : 1)) % App::FIELD_COUNT;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) saveSettingsFromModal(app);
    if (IsKeyPressed(KEY_ESCAPE) && app.modalCanCancel) closeSettings(app);

    // what the fields show: the token is masked unless "Show" is active; the
    // focused field gets a blinking caret
    const bool caretOn = ((int)(GetTime() * 2) % 2) == 0;
    for (int i = 0; i < App::FIELD_COUNT; ++i) {
        std::string shown = app.fieldValue[i];
        if (i == App::FIELD_TOKEN && !app.tokenVisible && shown.size() > 3)
            shown = shown.substr(0, 3) + std::string(shown.size() - 3, '*');
        if (shown.size() > 46) shown = "..." + shown.substr(shown.size() - 43);
        if (i == app.focusedField) shown += caretOn ? "|" : " ";
        app.fieldDisplay[i] = shown;
    }

    const std::string path = app.cfg.loadedFrom.empty() ? userConfigDir() + "config.json" : app.cfg.loadedFrom;
    app.modalPathLine = "Stored in " + textutil::toAscii(path);
}

// ---------------------------------------------------------------------------
// Model -> display strings
// ---------------------------------------------------------------------------
void applyFilter(App &app) {
    app.visible.clear();
    for (int i = 0; i < (int)app.rows.size(); ++i) {
        if (app.filter == F_ALL || (app.rows[i].task->sources & kFilterMask[app.filter])) app.visible.push_back(i);
    }
    app.filterDirty = false;
}

void rebuildRows(App &app) {
    using namespace textutil;
    clickup::sortTasks(app.tasks, app.sort);

    app.rows.clear();
    app.rows.reserve(app.tasks.size());
    for (int &c : app.counts) c = 0;

    const long long now = nowMs();
    const Clay_Color defaultPill{90, 96, 110, 255};

    for (const clickup::Task &t : app.tasks) {
        Row r;
        r.task = &t;

        std::string name = t.customId.empty() ? t.name : t.customId + "  " + t.name;
        r.name = truncate(toAscii(name), 160);
        if (r.name.empty()) r.name = "(untitled)";

        std::string status = toAscii(t.status);
        for (auto &ch : status) ch = (char)std::toupper((unsigned char)ch);
        r.status = truncate(status.empty() ? "?" : status, 14);

        std::string meta;
        if (t.dueDateMs > 0) meta += (t.dueDateMs < now ? "OVERDUE " : "Due ") + formatDate(t.dueDateMs);
        if (!t.priority.empty()) meta += (meta.empty() ? "" : "   |   ") + std::string("priority ") + toAscii(t.priority);
        std::string path;
        if (!t.spaceName.empty()) path += toAscii(t.spaceName);
        if (!t.folderName.empty() && t.folderName != "hidden") path += (path.empty() ? "" : " > ") + toAscii(t.folderName);
        if (!t.listName.empty()) path += (path.empty() ? "" : " > ") + toAscii(t.listName);
        if (!path.empty()) meta += (meta.empty() ? "" : "   |   ") + path;
        r.meta = truncate(meta, 180);

        // compact: due, bare priority word, list name only
        std::string mc;
        if (t.dueDateMs > 0) mc += (t.dueDateMs < now ? "OVERDUE " : "Due ") + formatDate(t.dueDateMs);
        if (!t.priority.empty()) mc += (mc.empty() ? "" : "  |  ") + toAscii(t.priority);
        if (!t.listName.empty()) mc += (mc.empty() ? "" : "  |  ") + toAscii(t.listName);
        r.metaCompact = truncate(mc, 90);

        std::vector<const char *> tags;
        if (t.sources & clickup::SRC_ASSIGNED) tags.push_back("assigned");
        if (t.sources & clickup::SRC_MENTIONED) tags.push_back("@mention");
        if (t.sources & clickup::SRC_COMMENTED) tags.push_back("my comment");
        for (size_t i = 0; i < tags.size(); ++i) r.tag += (i ? " + " : "") + std::string(tags[i]);

        if (!t.mentionSnippet.empty()) r.snippet = "\"" + truncate(toAscii(t.mentionSnippet), 120) + "\"";

        r.pill = parseHexColor(t.statusColor, defaultPill);
        r.pillText = textColorOn(r.pill);

        app.counts[F_ALL]++;
        if (t.sources & clickup::SRC_ASSIGNED) app.counts[F_ASSIGNED]++;
        if (t.sources & clickup::SRC_MENTIONED) app.counts[F_MENTIONED]++;
        if (t.sources & clickup::SRC_COMMENTED) app.counts[F_COMMENTED]++;

        app.rows.push_back(std::move(r));
    }
    app.filterDirty = true;
    app.detailDirty = true;
}

// Fills the detail pane strings and lays the status chips out into rows that
// fit the pane width (Clay has no wrapping flex container).
void updateDetail(App &app, float availWidth) {
    if (app.selectedId.empty()) return;
    if (!app.detailDirty && std::abs(availWidth - app.chipAvailWidth) < 1.0f) return;
    app.detailDirty = false;
    app.chipAvailWidth = availWidth;

    const clickup::Task *t = findTask(app, app.selectedId);
    if (!t) {
        app.selectedId.clear();
        return;
    }
    const Row *row = nullptr;
    for (const auto &r : app.rows)
        if (r.task == t) row = &r;

    using namespace textutil;
    app.detailTitle = truncate(toAscii(t->name), 300);
    if (app.detailTitle.empty()) app.detailTitle = "(untitled)";
    app.detailParent.clear();
    if (!app.navStack.empty()) {
        if (const clickup::Task *parent = findTask(app, app.navStack.back()))
            app.detailParent = "Subtask of: " + truncate(toAscii(parent->name), 80);
    }
    std::string statusUpper = toAscii(t->status);
    for (auto &ch : statusUpper) ch = (char)std::toupper((unsigned char)ch);
    app.detailStatus = row ? row->status : truncate(statusUpper.empty() ? "?" : statusUpper, 14);
    app.detailTag = row ? row->tag : "";
    app.detailPill = row ? row->pill : parseHexColor(t->statusColor, Clay_Color{90, 96, 110, 255});
    app.detailPillText = textColorOn(app.detailPill);

    std::string meta;
    if (!t->customId.empty()) meta += toAscii(t->customId);
    if (t->dueDateMs > 0) meta += (meta.empty() ? "" : "  |  ") + std::string(t->dueDateMs < nowMs() ? "OVERDUE " : "Due ") + formatDate(t->dueDateMs);
    if (!t->priority.empty()) meta += (meta.empty() ? "" : "  |  ") + std::string("priority ") + toAscii(t->priority);
    std::string path;
    if (!t->spaceName.empty()) path += toAscii(t->spaceName);
    if (!t->folderName.empty() && t->folderName != "hidden") path += (path.empty() ? "" : " > ") + toAscii(t->folderName);
    if (!t->listName.empty()) path += (path.empty() ? "" : " > ") + toAscii(t->listName);
    if (!path.empty()) meta += (meta.empty() ? "" : "\n") + path;
    app.detailMeta = meta;

    if (app.detailLoading && !t->detailLoaded) app.detailDescription = "Loading...";
    else if (t->description.empty()) app.detailDescription = t->detailLoaded ? "(no description)" : "";
    else app.detailDescription = truncate(tidyParagraphs(toAscii(t->description, true)), 6000);

    // subtasks
    app.subRows.clear();
    for (const clickup::Task &s : t->subtasks) {
        SubRow sr;
        sr.id = s.id;
        sr.url = s.url;
        sr.name = truncate(toAscii(s.name), 140);
        if (sr.name.empty()) sr.name = "(untitled)";
        std::string st = toAscii(s.status);
        for (auto &ch : st) ch = (char)std::toupper((unsigned char)ch);
        sr.status = truncate(st.empty() ? "?" : st, 12);
        if (s.dueDateMs > 0) sr.meta = std::string(s.dueDateMs < nowMs() ? "OVERDUE " : "Due ") + formatDate(s.dueDateMs);
        if (!s.priority.empty()) sr.meta += (sr.meta.empty() ? "" : "  |  ") + toAscii(s.priority);
        sr.pill = parseHexColor(s.statusColor, Clay_Color{90, 96, 110, 255});
        sr.pillText = textColorOn(sr.pill);
        app.subRows.push_back(std::move(sr));
    }
    app.subtasksLabel = "SUBTASKS (" + std::to_string(app.subRows.size()) + ")";

    if (!app.statusChanging.empty()) app.detailNote = "Updating status...";
    else if (!app.detailError.empty()) app.detailNote = truncate(toAscii(app.detailError), 200);
    else app.detailNote.clear();

    // status chips
    app.chipLabels.clear();
    app.chipColors.clear();
    app.chipCurrent.clear();
    app.chipRows.clear();
    auto it = app.statusesByList.find(t->listId);
    if (it != app.statusesByList.end()) {
        const std::string cur = lowerAscii(t->status);
        float x = 0;
        for (const auto &o : it->second) {
            std::string label = toAscii(o.status);
            for (auto &ch : label) ch = (char)std::toupper((unsigned char)ch);
            const float w = (float)label.size() * 7.6f + 24.0f;  // approx: 12px font + letterSpacing + padding
            if (app.chipRows.empty() || (x + w > availWidth && !app.chipRows.back().empty())) {
                app.chipRows.emplace_back();
                x = 0;
            }
            app.chipRows.back().push_back((int)app.chipLabels.size());
            x += w + 6;
            app.chipLabels.push_back(label);
            app.chipColors.push_back(parseHexColor(o.color, Clay_Color{120, 126, 140, 255}));
            app.chipCurrent.push_back(lowerAscii(o.status) == cur);
        }
    }
}

void pollBackground(App &app) {
    // 1) big refresh result
    std::unique_ptr<FetchResult> res;
    std::vector<MainFn> posted;
    {
        std::lock_guard<std::mutex> lock(app.shared->mu);
        if (app.shared->result) res = std::move(app.shared->result);
        posted.swap(app.shared->posted);
    }
    if (res && !res->cancelled) {
        app.lastFetchDone = GetTime();
        if (res->ok) {
            // keep details (description, subtasks) we already loaded
            std::unordered_map<std::string, clickup::Task> loaded;
            for (auto &t : app.tasks)
                if (t.detailLoaded) loaded[t.id] = std::move(t);
            app.me = res->me;
            app.tasks = std::move(res->tasks);
            for (auto &t : app.tasks) {
                auto d = loaded.find(t.id);
                if (d != loaded.end()) {
                    t.description = std::move(d->second.description);
                    t.subtasks = std::move(d->second.subtasks);
                    t.detailLoaded = true;
                }
            }
            app.errorLine.clear();
            app.lastUpdated = textutil::formatClock(std::time(nullptr));
            rebuildRows(app);
            std::fprintf(stderr, "[fetch] %zu tasks for %s\n", app.tasks.size(), app.me.email.c_str());
        } else {
            app.errorLine = res->error;
            std::fprintf(stderr, "[fetch] error: %s\n", res->error.c_str());
            if (res->error.find("401") != std::string::npos) openSettings(app, "ClickUp rejected this token.");
        }
    }
    // 2) small jobs
    for (auto &fn : posted) fn(app);
    reapJobs(app);
}

void updateStrings(App &app) {
    const bool busy = app.shared->busy.load();

    if (!app.me.email.empty()) app.userLine = (app.compact ? "" : "Signed in as ") + textutil::toAscii(app.me.email);
    else if (!app.cfg.error.empty()) app.userLine = "Not configured";
    else app.userLine = busy ? "Connecting..." : "Not connected";

    if (busy) {
        std::lock_guard<std::mutex> lock(app.shared->mu);
        app.statusLine = textutil::toAscii(app.shared->progress);
    } else if (!app.errorLine.empty()) {
        app.statusLine = textutil::truncate(textutil::toAscii(app.errorLine), 110);
    } else if (!app.lastUpdated.empty()) {
        app.statusLine = "Updated " + app.lastUpdated + "  |  " + std::to_string(app.tasks.size()) + " tasks";
    } else {
        app.statusLine.clear();
    }

    static const char *kShortName[F_COUNT] = {"All", "Assigned", "Mentions", "Comments"};
    for (int i = 0; i < F_COUNT; ++i) {
        app.tabLabels[i] = std::string(kFilterName[i]) + " (" + std::to_string(app.counts[i]) + ")";
        app.tabLabelsShort[i] = std::string(kShortName[i]) + " " + std::to_string(app.counts[i]);
    }
    app.sortLabel = kSortLabel[(int)app.sort];

    if (!app.cfg.error.empty()) app.emptyLine = textutil::toAscii(app.cfg.error);
    else if (busy && app.rows.empty()) app.emptyLine = "Loading...";
    else if (!app.errorLine.empty() && app.rows.empty()) app.emptyLine = textutil::toAscii(app.errorLine);
    else app.emptyLine = "Nothing here. No tasks match this filter.";
}

void cycleSort(App &app) {
    app.sort = (clickup::SortMode)(((int)app.sort + 1) % (int)clickup::SortMode::Count);
    rebuildRows(app);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
// Small pill-shaped button. Returns true when clicked this frame.
bool chipButton(Clay_ElementId id, const std::string &label, const Clay_TextElementConfig &text, Clay_Color bg,
                Clay_Color bgHover, Clay_Padding padding = {10, 10, 6, 6}) {
    bool clicked = false;
    CLAY(id, {.layout = {.padding = padding, .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = Clay_Hovered() ? bgHover : bg,
              .cornerRadius = CLAY_CORNER_RADIUS(6)}) {
        clicked = Clay_Hovered() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        CLAY_TEXT(S(label), text);
    }
    return clicked;
}

void layoutRefreshButton(App &app, bool busy, float w, float h, const Clay_TextElementConfig &label) {
    CLAY(CLAY_ID("RefreshBtn"), {.layout = {.sizing = {CLAY_SIZING_FIXED(w), CLAY_SIZING_FIXED(h)},
                                            .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                                 .backgroundColor = busy ? C_PANEL : (Clay_Hovered() ? C_ACCENT_H : C_ACCENT),
                                 .cornerRadius = CLAY_CORNER_RADIUS(6)}) {
        if (!busy && Clay_Hovered() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) startFetch(app);
        if (busy) CLAY_TEXT(CLAY_STRING("Loading..."), TAB_SM);
        else CLAY_TEXT(CLAY_STRING("Refresh"), label);
    }
}

void layoutSortChip(App &app) {
    if (chipButton(CLAY_ID("SortChip"), app.sortLabel, TAB_SM, C_CHIP, C_CHIP_H, Clay_Padding{9, 9, 5, 5}))
        cycleSort(app);
}

void layoutHeader(App &app) {
    const bool busy = app.shared->busy.load();

    if (app.compact) {
        // Narrow: title + button on one line, then account, then status + sort.
        CLAY(CLAY_ID("Header"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                            .childGap = 3,
                                            .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
            CLAY(CLAY_ID("HeaderTop"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                   .childGap = 8,
                                                   .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
                if (app.logoLoaded) {
                    CLAY(CLAY_ID("Logo"), {.layout = {.sizing = {CLAY_SIZING_FIXED(26), CLAY_SIZING_FIXED(26)}},
                                           .image = {.imageData = &app.logo}}) {}
                }
                CLAY(CLAY_ID("HeaderUser"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) {
                    CLAY_TEXT(S(app.userLine), MUTED_SM);
                }
                static const std::string kSettings = "Settings";
                if (chipButton(CLAY_ID("SettingsBtn"), kSettings, TAB_SM, C_CHIP, C_CHIP_H, Clay_Padding{9, 9, 7, 7}))
                    openSettings(app, "");
                layoutRefreshButton(app, busy, 88, 30, BUTTON_SM);
            }
            CLAY(CLAY_ID("HeaderStatusRow"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                         .childGap = 8,
                                                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
                CLAY(CLAY_ID("HeaderStatus"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) {
                    CLAY_TEXT(S(app.statusLine), app.errorLine.empty() || busy ? MUTED_SM : ERROR_SM);
                }
                layoutSortChip(app);
            }
        }
        return;
    }

    CLAY(CLAY_ID("Header"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                        .childGap = 16,
                                        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        if (app.logoLoaded) {
            CLAY(CLAY_ID("Logo"), {.layout = {.sizing = {CLAY_SIZING_FIXED(32), CLAY_SIZING_FIXED(32)}},
                                   .image = {.imageData = &app.logo}}) {}
        }
        CLAY(CLAY_ID("HeaderUser"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}}}) {
            CLAY_TEXT(S(app.userLine), MUTED_TEXT);
        }
        CLAY(CLAY_ID("HeaderStatus"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                  .childAlignment = {.x = CLAY_ALIGN_X_RIGHT}}}) {
            CLAY_TEXT(S(app.statusLine), app.errorLine.empty() || busy ? STATUS_TEXT : STATUS_ERR);
        }
        static const std::string kSettings = "Settings";
        if (chipButton(CLAY_ID("SettingsBtn"), kSettings, TAB_TEXT, C_CHIP, C_CHIP_H, Clay_Padding{12, 12, 9, 9}))
            openSettings(app, "");
        layoutRefreshButton(app, busy, 110, 36, BUTTON_TEXT);
    }
}

void layoutTabs(App &app) {
    const bool compact = app.compact;
    const uint16_t gap = compact ? 4 : 6;
    const Clay_Padding pad = compact ? Clay_Padding{8, 8, 5, 5} : Clay_Padding{12, 12, 6, 6};
    CLAY(CLAY_ID("Tabs"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                      .childGap = gap,
                                      .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        for (int i = 0; i < F_COUNT; ++i) {
            const bool active = app.filter == i;
            CLAY(CLAY_IDI("Tab", i), {.layout = {.padding = pad},
                                      .backgroundColor = active ? C_ACCENT : (Clay_Hovered() ? C_ROW_H : C_PANEL),
                                      .cornerRadius = CLAY_CORNER_RADIUS(6)}) {
                if (Clay_Hovered() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && app.filter != i) {
                    app.filter = i;
                    app.filterDirty = true;
                }
                if (compact) CLAY_TEXT(S(app.tabLabelsShort[i]), active ? TAB_SM_ACT : TAB_SM);
                else CLAY_TEXT(S(app.tabLabels[i]), active ? TAB_ACTIVE : TAB_TEXT);
            }
        }
        if (!compact) {
            CLAY(CLAY_ID("TabsSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            layoutSortChip(app);
        }
    }
}

void layoutTaskRow(App &app, const Row &r, int k) {
    const bool selected = app.selectedId == r.task->id;
    const Clay_Color bg = selected ? C_ROW_SEL : (Clay_Hovered() ? C_ROW_H : C_ROW);

    if (app.compact) {
        // Narrow: status + tag on the first line, then title, then one meta line.
        CLAY(CLAY_IDI("TaskRow", k), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                 .padding = {10, 10, 8, 9},
                                                 .childGap = 4,
                                                 .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                      .backgroundColor = selected ? C_ROW_SEL : (Clay_Hovered() ? C_ROW_H : C_ROW),
                                      .cornerRadius = CLAY_CORNER_RADIUS(8)}) {
            if (Clay_Hovered() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) openFromList(app, *r.task);

            CLAY(CLAY_IDI("RowTop", k), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                    .childGap = 8,
                                                    .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
                CLAY(CLAY_IDI("Pill", k), {.layout = {.padding = {8, 8, 3, 3},
                                                      .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                                           .backgroundColor = r.pill,
                                           .cornerRadius = CLAY_CORNER_RADIUS(4)}) {
                    CLAY_TEXT(S(r.status), Clay_TextElementConfig{.textColor = r.pillText, .fontId = 0, .fontSize = 11,
                                                                  .letterSpacing = 1, .wrapMode = CLAY_TEXT_WRAP_NONE});
                }
                CLAY(CLAY_IDI("Spacer", k), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
                CLAY(CLAY_IDI("Tag", k), {.layout = {.padding = {7, 7, 3, 3}},
                                          .backgroundColor = C_TAG_BG,
                                          .cornerRadius = CLAY_CORNER_RADIUS(4)}) {
                    CLAY_TEXT(S(r.tag), TAG_SM);
                }
            }
            CLAY_TEXT(S(r.name), BODY_SM);
            if (!r.metaCompact.empty()) CLAY_TEXT(S(r.metaCompact), MUTED_SM);
            if (!r.snippet.empty()) CLAY_TEXT(S(r.snippet), SNIPPET_SM);
        }
        return;
    }

    CLAY(CLAY_IDI("TaskRow", k), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                             .padding = {14, 14, 10, 10},
                                             .childGap = 14,
                                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
                                  .backgroundColor = bg,
                                  .cornerRadius = CLAY_CORNER_RADIUS(8)}) {
        if (Clay_Hovered() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) openFromList(app, *r.task);

        CLAY(CLAY_IDI("Pill", k), {.layout = {.sizing = {CLAY_SIZING_FIXED(112), CLAY_SIZING_FIT(0)},
                                              .padding = {8, 8, 5, 5},
                                              .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                                   .backgroundColor = r.pill,
                                   .cornerRadius = CLAY_CORNER_RADIUS(4)}) {
            CLAY_TEXT(S(r.status), Clay_TextElementConfig{.textColor = r.pillText, .fontId = 0, .fontSize = 12,
                                                          .letterSpacing = 1});
        }
        CLAY(CLAY_IDI("Body", k), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                              .childGap = 3,
                                              .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
            CLAY_TEXT(S(r.name), BODY_TEXT);
            CLAY_TEXT(S(r.meta), MUTED_TEXT);
            if (!r.snippet.empty()) CLAY_TEXT(S(r.snippet), SNIPPET_TEXT);
        }
        CLAY(CLAY_IDI("Tag", k), {.layout = {.padding = {8, 8, 4, 4}},
                                  .backgroundColor = C_TAG_BG,
                                  .cornerRadius = CLAY_CORNER_RADIUS(4)}) {
            CLAY_TEXT(S(r.tag), TAG_TEXT);
        }
    }
}

void layoutList(App &app) {
    if (app.filterDirty) applyFilter(app);
    CLAY(CLAY_ID("TaskList"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                          .childGap = 8,
                                          .layoutDirection = CLAY_TOP_TO_BOTTOM},
                               .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}}) {
        if (app.visible.empty()) {
            CLAY(CLAY_ID("Empty"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                               .padding = CLAY_PADDING_ALL(24)},
                                    .backgroundColor = C_PANEL,
                                    .cornerRadius = CLAY_CORNER_RADIUS(8)}) {
                CLAY_TEXT(S(app.emptyLine), (!app.cfg.error.empty() || !app.errorLine.empty()) ? ERROR_TEXT : MUTED_TEXT);
            }
        }
        for (int k = 0; k < (int)app.visible.size(); ++k) layoutTaskRow(app, app.rows[app.visible[k]], k);
    }
}

void layoutStatusChips(App &app) {
    const clickup::Task *t = findTask(app, app.selectedId);
    if (!t) return;
    const bool locked = !app.statusChanging.empty();
    int idx = 0;
    for (size_t row = 0; row < app.chipRows.size(); ++row) {
        CLAY(CLAY_IDI("ChipRow", (int)row), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                        .childGap = 6}}) {
            for (int i : app.chipRows[row]) {
                const bool current = app.chipCurrent[i];
                const Clay_Color col = app.chipColors[i];
                CLAY(CLAY_IDI("Chip", idx++), {.layout = {.padding = {10, 10, 5, 5},
                                                          .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                                               .backgroundColor = current ? col : withAlpha(col, Clay_Hovered() && !locked ? 120.0f : 45.0f),
                                               .cornerRadius = CLAY_CORNER_RADIUS(5),
                                               .border = {.color = col, .width = CLAY_BORDER_OUTSIDE(1)}}) {
                    if (!current && !locked && Clay_Hovered() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        // find the original (non-uppercased) status string
                        auto it = app.statusesByList.find(t->listId);
                        if (it != app.statusesByList.end() && i < (int)it->second.size())
                            changeStatus(app, t->id, it->second[i].status);
                    }
                    CLAY_TEXT(S(app.chipLabels[i]),
                              Clay_TextElementConfig{.textColor = current ? textColorOn(col) : C_TEXT, .fontId = 0,
                                                     .fontSize = 12, .letterSpacing = 1,
                                                     .wrapMode = CLAY_TEXT_WRAP_NONE});
                }
            }
        }
    }
}

void layoutDetailContent(App &app, const clickup::Task &t);

void layoutDetail(App &app, float paneWidth) {
    const bool compact = app.compact;
    const Clay_SizingAxis width = compact ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIXED(paneWidth);
    const clickup::Task *t = findTask(app, app.selectedId);
    if (!t) return;

    CLAY(CLAY_ID("Detail"), {.layout = {.sizing = {width, CLAY_SIZING_GROW(0)},
                                        .padding = CLAY_PADDING_ALL(14),
                                        .childGap = 10,
                                        .layoutDirection = CLAY_TOP_TO_BOTTOM},
                             .backgroundColor = C_PANEL,
                             .cornerRadius = CLAY_CORNER_RADIUS(8),
                             .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}}) {
        // toolbar
        CLAY(CLAY_ID("DetailTop"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                               .childGap = 8,
                                               .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            static const std::string kBack = "< Back", kOpen = "Open in ClickUp", kClose = "Close";
            // Back returns to the parent task when we navigated into a subtask; in the
            // narrow layout it also closes the pane when there is nothing to return to.
            if ((compact || !app.navStack.empty()) &&
                chipButton(CLAY_ID("BackBtn"), kBack, BUTTON_SM, C_CHIP, C_CHIP_H))
                goBack(app);
            CLAY(CLAY_ID("DetailTopSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            if (chipButton(CLAY_ID("OpenBtn"), kOpen, BUTTON_SM, C_ACCENT, C_ACCENT_H)) app.openUrl = t->url;
            if (!compact && chipButton(CLAY_ID("CloseBtn"), kClose, BUTTON_SM, C_CHIP, C_CHIP_H)) closeDetail(app);
        }
        // Never `return` inside a CLAY block: it would skip Clay__CloseElement.
        if (!app.selectedId.empty()) layoutDetailContent(app, *t);
    }
}

void layoutDetailContent(App &app, const clickup::Task &t) {
    {
        // status pill + source tag
        CLAY(CLAY_ID("DetailPills"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                 .childGap = 8,
                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            CLAY(CLAY_ID("DetailPill"), {.layout = {.padding = {8, 8, 4, 4}},
                                         .backgroundColor = app.detailPill,
                                         .cornerRadius = CLAY_CORNER_RADIUS(4)}) {
                CLAY_TEXT(S(app.detailStatus), Clay_TextElementConfig{.textColor = app.detailPillText, .fontId = 0,
                                                                      .fontSize = 11, .letterSpacing = 1,
                                                                      .wrapMode = CLAY_TEXT_WRAP_NONE});
            }
            if (!app.detailTag.empty()) {
                CLAY(CLAY_ID("DetailTag"), {.layout = {.padding = {7, 7, 3, 3}},
                                            .backgroundColor = C_TAG_BG,
                                            .cornerRadius = CLAY_CORNER_RADIUS(4)}) {
                    CLAY_TEXT(S(app.detailTag), TAG_SM);
                }
            }
        }

        if (!app.detailParent.empty()) CLAY_TEXT(S(app.detailParent), MUTED_SM);
        CLAY_TEXT(S(app.detailTitle), DETAIL_TITLE);
        if (!app.detailMeta.empty()) CLAY_TEXT(S(app.detailMeta), MUTED_SM);

        // status switcher
        CLAY(CLAY_ID("StatusSection"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                   .padding = {0, 0, 6, 0},
                                                   .childGap = 6,
                                                   .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
            CLAY_TEXT(CLAY_STRING("STATUS"), LABEL_TEXT);
            if (app.chipRows.empty()) {
                if (app.detailLoading) CLAY_TEXT(CLAY_STRING("Loading statuses..."), MUTED_SM);
                else CLAY_TEXT(CLAY_STRING("No status list available for this task."), MUTED_SM);
            } else {
                layoutStatusChips(app);
            }
            if (!app.detailNote.empty())
                CLAY_TEXT(S(app.detailNote), app.detailError.empty() ? MUTED_SM : ERROR_SM);
        }

        // subtasks
        if (!app.subRows.empty()) {
            CLAY(CLAY_ID("SubSection"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                    .padding = {0, 0, 6, 0},
                                                    .childGap = 6,
                                                    .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                CLAY_TEXT(S(app.subtasksLabel), LABEL_TEXT);
                for (int i = 0; i < (int)app.subRows.size(); ++i) {
                    const SubRow &sr = app.subRows[i];
                    CLAY(CLAY_IDI("Sub", i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                         .padding = {8, 8, 6, 6},
                                                         .childGap = 8,
                                                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
                                              .backgroundColor = Clay_Hovered() ? C_ROW_H : C_ROW,
                                              .cornerRadius = CLAY_CORNER_RADIUS(6)}) {
                        if (Clay_Hovered() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) openSubtask(app, t, (size_t)i);
                        CLAY(CLAY_IDI("SubPill", i), {.layout = {.padding = {6, 6, 2, 2}},
                                                      .backgroundColor = sr.pill,
                                                      .cornerRadius = CLAY_CORNER_RADIUS(3)}) {
                            CLAY_TEXT(S(sr.status), Clay_TextElementConfig{.textColor = sr.pillText, .fontId = 0,
                                                                           .fontSize = 10, .letterSpacing = 1,
                                                                           .wrapMode = CLAY_TEXT_WRAP_NONE});
                        }
                        CLAY(CLAY_IDI("SubBody", i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                 .childGap = 2,
                                                                 .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                            CLAY_TEXT(S(sr.name), Clay_TextElementConfig{.textColor = C_TEXT, .fontId = 0, .fontSize = 13});
                            if (!sr.meta.empty())
                                CLAY_TEXT(S(sr.meta), Clay_TextElementConfig{.textColor = C_MUTED, .fontId = 0, .fontSize = 12});
                        }
                    }
                }
            }
        }

        // description
        CLAY(CLAY_ID("DescSection"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                 .padding = {0, 0, 6, 0},
                                                 .childGap = 6,
                                                 .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
            CLAY_TEXT(CLAY_STRING("DESCRIPTION"), LABEL_TEXT);
            if (!app.detailDescription.empty()) CLAY_TEXT(S(app.detailDescription), DESC_TEXT);
        }
    }
}

void layoutBody(App &app) {
    const bool showDetail = !app.selectedId.empty();
    const float screenW = (float)GetScreenWidth();

    if (app.compact) {
        updateDetail(app, screenW - 2 * 10 - 2 * 14);
        if (showDetail) layoutDetail(app, 0);
        else layoutList(app);
        return;
    }

    const float paneWidth = std::min(420.0f, std::max(300.0f, screenW * 0.42f));
    updateDetail(app, paneWidth - 2 * 14);
    CLAY(CLAY_ID("Body"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = 12}}) {
        layoutList(app);
        if (showDetail) layoutDetail(app, paneWidth);
    }
}

// One labelled text field of the settings dialog. Clicking it moves the focus.
void layoutSettingsField(App &app, int idx, const char *label, const std::string &placeholder) {
    const bool focused = app.focusedField == idx;
    CLAY(CLAY_IDI("FieldBox", idx), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                .childGap = 4,
                                                .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
        const Clay_String labelStr{true, (int32_t)std::strlen(label), label};  // literal, lives forever
        CLAY_TEXT(labelStr, LABEL_TEXT);
        CLAY(CLAY_IDI("Field", idx), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(38)},
                                                 .padding = {12, 12, 0, 0},
                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
                                      .backgroundColor = C_BG,
                                      .cornerRadius = CLAY_CORNER_RADIUS(6),
                                      .clip = {.horizontal = true},
                                      .border = {.color = focused ? C_ACCENT : C_ROW_H, .width = CLAY_BORDER_OUTSIDE(1)}}) {
            if (Clay_Hovered() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) app.focusedField = idx;
            if (app.fieldValue[idx].empty() && !focused)
                CLAY_TEXT(S(placeholder), Clay_TextElementConfig{.textColor = C_MUTED, .fontId = 0, .fontSize = 15,
                                                                 .wrapMode = CLAY_TEXT_WRAP_NONE});
            else
                CLAY_TEXT(S(app.fieldDisplay[idx]), Clay_TextElementConfig{.textColor = C_TEXT, .fontId = 0,
                                                                          .fontSize = 15, .letterSpacing = 1,
                                                                          .wrapMode = CLAY_TEXT_WRAP_NONE});
        }
    }
}

// Settings dialog. A floating element attached to the root covers the whole
// window and captures the pointer, so nothing behind it reacts while it is open.
void layoutSettingsModal(App &app) {
    const float w = (float)GetScreenWidth(), h = (float)GetScreenHeight();
    const float boxW = std::min(480.0f, w - 24.0f);
    static const std::string kPaste = "Paste", kShow = "Show token", kHide = "Hide token", kClear = "Clear",
                             kCancel = "Cancel", kSave = "Save";
    static const std::string kPhToken = "pk_...", kPhEmail = "you@company.com (optional)",
                             kPhUser = "Your ClickUp username (optional)";

    CLAY(CLAY_ID("ModalBackdrop"), {.layout = {.sizing = {CLAY_SIZING_FIXED(w), CLAY_SIZING_FIXED(h)},
                                               .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                                    .backgroundColor = {0, 0, 0, 170},
                                    .floating = {.zIndex = 10, .attachTo = CLAY_ATTACH_TO_ROOT}}) {
        CLAY(CLAY_ID("Modal"), {.layout = {.sizing = {CLAY_SIZING_FIXED(boxW), CLAY_SIZING_FIT(0)},
                                           .padding = CLAY_PADDING_ALL(18),
                                           .childGap = 12,
                                           .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                .backgroundColor = C_PANEL,
                                .cornerRadius = CLAY_CORNER_RADIUS(10),
                                .border = {.color = C_ROW_H, .width = CLAY_BORDER_OUTSIDE(1)}}) {
            CLAY(CLAY_ID("ModalTitleRow"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                       .childGap = 12,
                                                       .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
                if (app.logoLoaded) {
                    CLAY(CLAY_ID("ModalLogo"), {.layout = {.sizing = {CLAY_SIZING_FIXED(44), CLAY_SIZING_FIXED(44)}},
                                                .image = {.imageData = &app.logo}}) {}
                }
                CLAY(CLAY_ID("ModalTitleCol"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                           .childGap = 2,
                                                           .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                    CLAY_TEXT(CLAY_STRING("Settings"), DETAIL_TITLE);
                    CLAY_TEXT(CLAY_STRING("ClickUp connection"), MUTED_SM);
                }
            }
            CLAY_TEXT(CLAY_STRING("In ClickUp open Settings > Apps > API Token and press Generate. "
                                  "The token starts with pk_ and is only stored on this computer."),
                      MUTED_SM);

            layoutSettingsField(app, App::FIELD_TOKEN, "API TOKEN", kPhToken);
            CLAY(CLAY_ID("ModalTools"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .childGap = 6}}) {
                if (chipButton(CLAY_ID("PasteBtn"), kPaste, TAB_SM, C_CHIP, C_CHIP_H)) appendToField(app, GetClipboardText());
                if (chipButton(CLAY_ID("ShowBtn"), app.tokenVisible ? kHide : kShow, TAB_SM, C_CHIP, C_CHIP_H))
                    app.tokenVisible = !app.tokenVisible;
                if (chipButton(CLAY_ID("ClearBtn"), kClear, TAB_SM, C_CHIP, C_CHIP_H)) app.fieldValue[app.focusedField].clear();
            }

            CLAY_TEXT(CLAY_STRING("Mentions are matched against the profile of the token. Set an email or username "
                                  "here to match against those instead (for example a shared or secondary account)."),
                      MUTED_SM);
            layoutSettingsField(app, App::FIELD_EMAIL, "EMAIL", kPhEmail);
            layoutSettingsField(app, App::FIELD_USERNAME, "USERNAME", kPhUser);

            if (!app.modalError.empty()) CLAY_TEXT(S(app.modalError), ERROR_SM);
            CLAY_TEXT(S(app.modalPathLine), MUTED_SM);

            CLAY(CLAY_ID("ModalActions"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                      .childGap = 8,
                                                      .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
                if (app.modalCanCancel) CLAY_TEXT(CLAY_STRING("Tab: next field   Enter: save   Esc: cancel"), MUTED_SM);
                else CLAY_TEXT(CLAY_STRING("Tab: next field   Enter: save"), MUTED_SM);
                CLAY(CLAY_ID("ModalSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
                if (app.modalCanCancel && chipButton(CLAY_ID("CancelBtn"), kCancel, BUTTON_SM, C_CHIP, C_CHIP_H))
                    closeSettings(app);
                if (chipButton(CLAY_ID("SaveBtn"), kSave, BUTTON_SM, C_ACCENT, C_ACCENT_H)) saveSettingsFromModal(app);
            }
        }
    }
}

Clay_RenderCommandArray buildLayout(App &app) {
    const uint16_t pad = app.compact ? 10 : 16;
    const uint16_t gap = app.compact ? 8 : 12;
    Clay_BeginLayout();
    CLAY(CLAY_ID("Root"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                      .padding = CLAY_PADDING_ALL(pad),
                                      .childGap = gap,
                                      .layoutDirection = CLAY_TOP_TO_BOTTOM},
                           .backgroundColor = C_BG}) {
        layoutHeader(app);
        layoutTabs(app);
        layoutBody(app);
        if (app.compact) CLAY_TEXT(S(app.footerShort), MUTED_SM);
        else CLAY_TEXT(S(app.footerLine), MUTED_TEXT);
        if (app.modalOpen) layoutSettingsModal(app);
    }
    return Clay_EndLayout(GetFrameTime());
}

// ---------------------------------------------------------------------------
// Frame / main
// ---------------------------------------------------------------------------
void frame(App &app) {
    pollBackground(app);

    if (app.modalOpen) {
        handleModalInput(app);  // the modal owns the keyboard while open
    } else {
        if (IsKeyPressed(KEY_R)) startFetch(app);
        if (IsKeyPressed(KEY_S)) cycleSort(app);
        if (IsKeyPressed(KEY_ESCAPE)) goBack(app);
        if (IsKeyPressed(KEY_D)) {
            app.debug = !app.debug;
            Clay_SetDebugModeEnabled(app.debug);
        }
    }
    if (app.cfg.refreshMinutes > 0 && app.lastFetchDone >= 0 && !app.shared->busy.load() &&
        GetTime() - app.lastFetchDone > app.cfg.refreshMinutes * 60.0) {
        startFetch(app);
    }

    app.compact = GetScreenWidth() < kCompactWidth;
    updateStrings(app);

    Clay_SetLayoutDimensions(Clay_Dimensions{(float)GetScreenWidth(), (float)GetScreenHeight()});
    Vector2 mouse = GetMousePosition();
    Clay_SetPointerState(Clay_Vector2{mouse.x, mouse.y}, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    Vector2 wheel = GetMouseWheelMoveV();
    Clay_UpdateScrollContainers(false, Clay_Vector2{wheel.x * 4.0f, wheel.y * 4.0f}, GetFrameTime());

    Clay_RenderCommandArray commands = buildLayout(app);

    BeginDrawing();
    ClearBackground(Color{24, 26, 31, 255});
    Clay_Raylib_Render(commands, app.fonts);
    EndDrawing();

    if (!app.openUrl.empty()) {
        OpenURL(app.openUrl.c_str());
        app.openUrl.clear();
    }

    // Remember the window geometry a couple of seconds after it changes, so a
    // resized sidebar survives even if the process is killed instead of closed.
    if (GetTime() - app.lastGeometryCheck > 2.0) {
        app.lastGeometryCheck = GetTime();
        saveWindowState(app, false);
    }
}

// `apcy --demo`: sample data so the UI can be tried without a token.
void loadDemo(App &app) {
    using namespace clickup;
    const long long day = 24LL * 3600LL * 1000LL;
    const long long now = textutil::nowMs();
    std::vector<Task> v;
    auto add = [&](const char *id, const char *name, const char *status, const char *color, const char *prio,
                   long long due, unsigned src, const char *snippet, const char *space, const char *list,
                   const char *description) {
        Task t;
        t.id = id; t.customId = std::string("DEMO-") + id; t.name = name; t.status = status; t.statusColor = color;
        t.priority = prio; t.dueDateMs = due; t.dateUpdatedMs = now - (long long)v.size() * 3600000LL;
        t.sources = src; t.mentionSnippet = snippet; t.spaceName = space; t.listName = list; t.listId = "demo-list";
        t.url = "https://app.clickup.com/"; t.description = description; t.detailLoaded = true;
        v.push_back(t);
    };
    add("101", "Revisión de la facturación de septiembre", "in progress", "#4194f6", "high", now - 2 * day,
        SRC_ASSIGNED, "", "Finanzas", "Cierre mensual",
        "Revisar las facturas emitidas durante septiembre y cruzarlas con los pagos recibidos.\n\n"
        "Pendientes:\n- Conciliar con el banco\n- Validar notas de crédito\n- Enviar resumen a dirección");
    add("102", "Migrate the auth service to the new gateway", "to do", "#d3d3d3", "urgent", now + day,
        SRC_ASSIGNED | SRC_MENTIONED, "@you can you take a look at the retry policy before Friday?", "Engineering",
        "Backend",
        "The legacy gateway is being decommissioned at the end of the month. Move the auth service behind the new "
        "gateway, keep the same public paths, and make sure the retry policy does not duplicate token refreshes.\n\n"
        "Acceptance criteria:\n1. All auth endpoints reachable through the new gateway\n2. p95 latency under 120 ms\n"
        "3. Runbook updated");
    add("103", "Q4 planning deck – first draft", "review", "#f9d900", "normal", now + 5 * day, SRC_MENTIONED,
        "@author:you@example.com please add the hiring numbers", "Management", "Planning",
        "First draft of the Q4 planning deck. Needs hiring numbers and the updated roadmap slide.");
    add("104", "Customer bug: export CSV fails with emoji 🚀 in names", "blocked", "#e50000", "", 0,
        SRC_COMMENTED, "Reproduced on staging, the encoder chokes on 4-byte UTF-8.", "Support", "Escalations", "");
    add("105", "Write onboarding guide for new designers", "to do", "#87909e", "low", 0, SRC_ASSIGNED | SRC_COMMENTED,
        "Started an outline, see attached doc.", "Design", "Team",
        "Outline:\n- Tools and access\n- Design system overview\n- Review process\n- First week checklist");

    auto sub = [&](const char *id, const char *name, const char *status, const char *color, long long due) {
        Task s;
        s.id = id; s.name = name; s.status = status; s.statusColor = color; s.dueDateMs = due;
        s.url = "https://app.clickup.com/";
        return s;
    };
    v[1].subtasks = {
        sub("201", "Inventory of auth endpoints", "done", "#6bc950", 0),
        sub("202", "Configure routes on the new gateway", "in progress", "#4194f6", now + day),
        sub("203", "Load test and compare latency", "to do", "#d3d3d3", now + 2 * day),
    };

    app.demo = true;
    app.cfg.error.clear();
    app.me = {"0", "demo", "demo@example.com"};
    app.tasks = std::move(v);
    app.statusesByList["demo-list"] = {
        {"to do", "#d3d3d3", "open", 0},      {"in progress", "#4194f6", "custom", 1},
        {"review", "#f9d900", "custom", 2},   {"blocked", "#e50000", "custom", 3},
        {"done", "#6bc950", "closed", 4},
    };
    app.lastUpdated = textutil::formatClock(std::time(nullptr));
    rebuildRows(app);
    if (clickup::Task *t = findTask(app, "102")) selectTask(app, *t);  // show the detail pane right away
    // test hook: APCY_DEMO_SUBTASK=1 starts inside the second subtask of DEMO-102
    if (std::getenv("APCY_DEMO_SUBTASK"))
        if (clickup::Task *t = findTask(app, "102")) openSubtask(app, *t, 1);
}

// Window geometry is remembered in window.json next to config.json so the app
// reopens where you left it (e.g. as a narrow sidebar).
std::string dirOf(const std::string &path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? std::string() : path.substr(0, p + 1);
}

std::string windowStatePath(const App &app) {
    // next to the config file that was used; otherwise the per-user config dir
    // (an installed app dir is usually read-only), falling back to the app dir.
    std::string dir;
    if (!app.cfg.loadedFrom.empty()) {
        dir = dirOf(app.cfg.loadedFrom);  // "" means the current directory
    } else {
        dir = userConfigDir();
        if (dir.empty()) dir = app.appDir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    return dir + "window.json";
}

void restoreWindowState(const std::string &path) {
    std::ifstream in(path);
    if (!in) return;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    const int w = j.value("w", 0), h = j.value("h", 0);
    if (w >= 320 && h >= 400 && w <= 10000 && h <= 10000) SetWindowSize(w, h);
    if (j.contains("x") && j.contains("y")) {
        const int x = j.value("x", 0), y = j.value("y", 0);
        if (x > -10000 && x < 20000 && y > -10000 && y < 20000) SetWindowPosition(x, y);
    }
}

void saveWindowState(App &app, bool force) {
    if (IsWindowMinimized() || IsWindowFullscreen()) return;
    const Vector2 pos = GetWindowPosition();
    const int geo[4] = {(int)pos.x, (int)pos.y, GetScreenWidth(), GetScreenHeight()};
    bool changed = force;
    for (int i = 0; i < 4; ++i) changed = changed || geo[i] != app.savedGeometry[i];
    if (!changed) return;

    nlohmann::json j = {{"x", geo[0]}, {"y", geo[1]}, {"w", geo[2]}, {"h", geo[3]}};
    std::ofstream out(windowStatePath(app));
    if (!out) return;
    out << j.dump(2) << "\n";
    for (int i = 0; i < 4; ++i) app.savedGeometry[i] = geo[i];
}

// Resources are searched in the layouts produced by the build and by the
// installers: next to the executable, in a Linux install prefix, in a macOS
// .app bundle, and in the current directory.
std::string findResource(const std::string &name, const std::string &appDir) {
    const std::string candidates[] = {
        appDir + "resources/" + name,
        appDir + "../share/apcy/resources/" + name,
        appDir + "../Resources/" + name,
        "resources/" + name,
    };
    for (const auto &c : candidates)
        if (FileExists(c.c_str())) return c;
    return candidates[0];
}

}  // namespace

int main(int argc, char **argv) {
    HttpClient::globalInit();

    SetTraceLogLevel(LOG_WARNING);
    Clay_Raylib_Initialize(980, 700, "apcy",
                           FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIGHDPI);
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);  // Esc closes the detail pane instead of the app

    App app;
    const std::string appDir = GetApplicationDirectory();
    app.appDir = appDir;
    app.cfg = loadConfig(argc, argv, appDir);
    SetWindowMinSize(320, 420);
    restoreWindowState(windowStatePath(app));
    if (!app.cfg.error.empty()) std::fprintf(stderr, "[config] %s\n", app.cfg.error.c_str());
    else std::fprintf(stderr, "[config] using %s\n", app.cfg.loadedFrom.empty() ? "environment" : app.cfg.loadedFrom.c_str());

    const std::string fontPath = findResource("Roboto-Regular.ttf", appDir);
    app.fonts[0] = LoadFontEx(fontPath.c_str(), 40, nullptr, 95);
    SetTextureFilter(app.fonts[0].texture, TEXTURE_FILTER_BILINEAR);

    // logo: header texture + window icon (the icon is ignored on macOS, where the bundle icon is used)
    const std::string logoPath = findResource("logo.png", appDir);
    if (FileExists(logoPath.c_str())) {
        app.logo = LoadTexture(logoPath.c_str());
        app.logoLoaded = app.logo.id != 0;
        if (app.logoLoaded) SetTextureFilter(app.logo, TEXTURE_FILTER_BILINEAR);
    }
#ifndef __APPLE__  // macOS windows have no per-window icon; the .app bundle carries apcy.icns
    const std::string iconPath = findResource("logo-256.png", appDir);
    if (FileExists(iconPath.c_str())) {
        Image icon = LoadImage(iconPath.c_str());
        if (icon.data) {
            SetWindowIcon(icon);
            UnloadImage(icon);
        }
    }
#endif

    Clay_SetMaxElementCount(16384);
    Clay_SetMaxMeasureTextCacheWordCount(65536);
    uint32_t arenaSize = Clay_MinMemorySize();
    void *arenaMemory = std::malloc(arenaSize);
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arenaSize, arenaMemory);
    Clay_Initialize(arena, Clay_Dimensions{(float)GetScreenWidth(), (float)GetScreenHeight()},
                    Clay_ErrorHandler{handleClayErrors, nullptr});
    ClayBridge_SetMeasureText(app.fonts);

    bool demo = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--demo") demo = true;

    if (demo) loadDemo(app);
    else if (!app.cfg.token.empty()) startFetch(app);
    else openSettings(app, "");  // first run: ask for the token right away

    while (!WindowShouldClose()) {
        frame(app);
        if (g_reinitClay) {
            g_reinitClay = false;
            std::free(arenaMemory);
            arenaSize = Clay_MinMemorySize();
            arenaMemory = std::malloc(arenaSize);
            arena = Clay_CreateArenaWithCapacityAndMemory(arenaSize, arenaMemory);
            Clay_Initialize(arena, Clay_Dimensions{(float)GetScreenWidth(), (float)GetScreenHeight()},
                            Clay_ErrorHandler{handleClayErrors, nullptr});
            ClayBridge_SetMeasureText(app.fonts);
            Clay_SetDebugModeEnabled(app.debug);
        }
    }

    saveWindowState(app, false);

    app.shared->cancel = true;
    if (app.worker.joinable()) app.worker.join();
    for (auto &j : app.jobs)
        if (j.th.joinable()) j.th.join();

    if (app.logoLoaded) UnloadTexture(app.logo);
    UnloadFont(app.fonts[0]);
    Clay_Raylib_Close();
    std::free(arenaMemory);
    HttpClient::globalCleanup();
    return 0;
}
