# apcy

A small desktop TO-DO style window that lists the ClickUp tasks that concern you:

- tasks **assigned to you**
- tasks where a comment **@mentions you** (or matches a configurable text pattern such as `@author:you@company.com`)
- tasks where **you wrote a comment**

Click a row to see the task description and switch its status with one click; "Open in ClickUp" jumps to the
browser. Works on Windows and Linux (and macOS).

Built with C++20, [Clay](https://github.com/nicbarker/clay) for layout, [raylib](https://www.raylib.com/) for
window/rendering, libcurl for HTTPS and nlohmann/json.

## 1. Get a ClickUp API token

ClickUp -> your avatar (bottom left) -> **Settings** -> **Apps** -> *API Token* -> **Generate**.
The token starts with `pk_`.

## 2. Configure

Copy `config.example.json` to `config.json` (next to the executable, or in the directory you launch from)
and paste the token:

```json
{
  "token": "pk_your_personal_api_token",
  "team_id": "",
  "include_closed": false,
  "comment_scan_days": 14,
  "max_comment_scan": 150,
  "refresh_minutes": 10,
  "mention_patterns": ["@{username}", "@author:{email}", "{email}"]
}
```

| key | meaning |
|-----|---------|
| `token` | personal API token. `CLICKUP_API_TOKEN` env var overrides it. |
| `team_id` | restrict to one workspace id. Empty = all workspaces the token can see. |
| `include_closed` | also list tasks in closed statuses. |
| `comment_scan_days` | how far back (by task update date) to look for comments. `0` disables comment scanning. |
| `max_comment_scan` | upper bound of tasks per workspace whose comments are fetched (one request each, ClickUp allows ~100 requests/minute). |
| `refresh_minutes` | auto refresh interval. `0` disables. |
| `mention_patterns` | case-insensitive text patterns that count as a mention. `{username}` and `{email}` are replaced with your ClickUp profile values. Structured @mentions are detected regardless of these. |

You can also point to a file with `apcy --config /path/to/config.json` or `APCY_CONFIG=/path/to/config.json`.

To look at the UI without a token, run `apcy --demo` (five sample tasks, no network access).

## 3. Build

CMake 3.24+ downloads raylib, Clay and nlohmann/json automatically. libcurl is taken from the system when present,
otherwise it is built from source too. A C++20 compiler with designated initializers is required: GCC 10+,
Clang 12+ or Visual Studio 2022.

### Linux (Debian/Ubuntu)

```bash
sudo apt install build-essential cmake git libcurl4-openssl-dev \
     libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev libwayland-dev libxkbcommon-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/apcy
```

Fedora: `sudo dnf install cmake gcc-c++ git libcurl-devel libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel mesa-libGL-devel wayland-devel libxkbcommon-devel`.

### Windows (Visual Studio 2022)

Open a *x64 Native Tools* prompt (or use the CMake integration in VS / VS Code):

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\apcy.exe
```

Without vcpkg, CMake builds libcurl from source using Windows Schannel for TLS, so no OpenSSL is needed.
If you have vcpkg, `-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake` with the `curl`
port installed is picked up automatically and speeds up the build.

### macOS

```bash
brew install cmake
cmake -S . -B build && cmake --build build -j && ./build/apcy
```

## Packaging

`cpack` produces a redistributable package from a Release build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release        # macOS: add -DAPCY_BUNDLE=ON for an .app/.dmg
cmake --build build --config Release --parallel
cd build && cpack -C Release
```

| platform | output | layout |
|----------|--------|--------|
| Windows | `apcy-0.1.0-win64.zip` | portable: `apcy.exe`, `resources/`, `config.example.json`, `README.md` (no console window) |
| Linux | `apcy-0.1.0-linux-x86_64.tar.gz` and `apcy_0.1.0_amd64.deb` | `bin/apcy`, `share/apcy/resources/`, `share/applications/apcy.desktop` |
| macOS | `apcy-0.1.0-macos-arm64.dmg` (with `-DAPCY_BUNDLE=ON`) | `apcy.app` with the font inside the bundle |

The executable finds its resources in all of these layouts. For an installed copy put `config.json` in the
per-user directory instead of next to the binary: `%APPDATA%\apcy\config.json` on Windows,
`~/.config/apcy/config.json` on Linux and macOS. `window.json` is written to the same place.

[.github/workflows/build.yml](.github/workflows/build.yml) builds and packages all three platforms on every push
and attaches the packages to a GitHub release when you push a tag such as `v0.1.0`.

## Usage

| action | how |
|--------|-----|
| show details (description, subtasks, status buttons) | click the row |
| open a subtask | click it in the detail pane; `< Back` or `Esc` returns to the parent task |
| change status | click a status chip in the detail pane (PUT to ClickUp, list updates when the API confirms) |
| open task in the browser | `Open in ClickUp` button in the detail pane |
| close details | `Close`, or `< Back` / `Esc` when there is no parent task to return to |
| sort | `Sort:` chip or `S` cycles due date -> priority -> recently updated -> status |
| refresh | `Refresh` button or `R` |
| filter | tabs: All / Assigned / Mentioned / My comments |
| scroll | mouse wheel / trackpad |
| Clay layout debugger | `D` |

Windows narrower than about 620 px switch to a compact sidebar layout (stacked header, short tab labels, status
and tag on one line above the title). The window size and position are saved to `window.json` next to
`config.json` on exit and restored on the next start.

## How the data is fetched

1. `GET /user` to learn your id, username and email.
2. `GET /team` (or the configured `team_id`).
3. Per workspace: `GET /team/{id}/task?assignees[]=<me>` (paged) -> **assigned**.
4. Per workspace: `GET /team/{id}/task?date_updated_gt=<now - comment_scan_days>` (paged, capped by
   `max_comment_scan`), then `GET /task/{id}/comment` for each. A comment counts as a mention when it contains a
   structured tag of your user, or when its text contains one of `mention_patterns`. A comment authored by you marks
   the task as **my comment**.
5. Results are merged by task id and sorted (default: overdue / due soonest first, then most recently updated).
6. Selecting a task calls `GET /task/{id}?include_subtasks=true` for the description and its subtasks, and
   `GET /list/{list_id}` once per list for its status workflow. Clicking a status chip sends `PUT /task/{id}` with
   `{"status": "..."}`.

Changing a status is the only write the app performs; everything else is read-only.

## Notes / limits

- Clay's stock raylib renderer measures text with an ASCII glyph table, so titles are transliterated to ASCII
  (accents removed, unknown symbols shown as `?`).
- Comment scanning is bounded by `max_comment_scan` to stay under ClickUp's rate limit. On HTTP 429 the client waits
  for `Retry-After` and retries.
- Tasks in archived lists are not returned by the ClickUp "filtered team tasks" endpoint.

## Project layout

```
CMakeLists.txt          dependencies + build
config.example.json     copy to config.json
resources/              Roboto font (Apache 2.0)
src/main.cpp            Clay UI, frame loop, background fetch thread
src/clickup.*           ClickUp API client and the assigned/mention/comment query
src/http.*              libcurl GET wrapper
src/config.*            config.json / env loading
src/textutil.hpp        UTF-8 -> ASCII, date formatting
src/clay_impl.c         CLAY_IMPLEMENTATION + Clay's raylib renderer (C)
src/clay_bridge.h       C declarations used from C++
```
