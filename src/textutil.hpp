// Small text helpers used by the UI layer.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace textutil {

// Maps a Unicode code point to a printable ASCII replacement. Clay's Raylib
// renderer measures text with a 95-glyph ASCII font table, so anything
// outside 0x20..0x7E must be replaced before it reaches the layout.
inline const char *asciiFor(uint32_t cp) {
    switch (cp) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return "A";
        case 0xC7: return "C";
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: return "E";
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: return "I";
        case 0xD1: return "N";
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return "O";
        case 0xD9: case 0xDA: case 0xDB: case 0xDC: return "U";
        case 0xDD: return "Y";
        case 0xDF: return "ss";
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return "a";
        case 0xE7: return "c";
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: return "e";
        case 0xEC: case 0xED: case 0xEE: case 0xEF: return "i";
        case 0xF1: return "n";
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: return "o";
        case 0xF9: case 0xFA: case 0xFB: case 0xFC: return "u";
        case 0xFD: case 0xFF: return "y";
        case 0x2013: case 0x2014: return "-";
        case 0x2018: case 0x2019: return "'";
        case 0x201C: case 0x201D: return "\"";
        case 0x2026: return "...";
        case 0x2022: return "*";
        case 0xA0: return " ";
        default: return nullptr;
    }
}

// UTF-8 -> printable ASCII (accents stripped, unknown symbols become '?').
// Control characters (tabs, newlines) become spaces so titles stay one line,
// unless keepNewlines is set (multi-line descriptions).
inline std::string toAscii(const std::string &in, bool keepNewlines = false) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) {
            if (c == '\n' && keepNewlines) out.push_back('\n');
            else if (c == '\r') { /* drop */ }
            else out.push_back((c < 0x20 || c == 0x7F) ? ' ' : (char)c);
            ++i;
            continue;
        }
        uint32_t cp = 0;
        int len = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { out.push_back('?'); ++i; continue; }
        if (i + len > in.size()) { out.push_back('?'); break; }
        for (int k = 1; k < len; ++k) cp = (cp << 6) | ((unsigned char)in[i + k] & 0x3F);
        i += len;
        if (const char *r = asciiFor(cp)) out += r;
        else if (cp >= 0x1F000) { /* emoji: drop silently */ }
        else out.push_back('?');
    }
    return out;
}

// Collapses runs of 3+ newlines to 2 and trims surrounding whitespace.
inline std::string tidyParagraphs(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    int newlines = 0;
    for (char c : in) {
        if (c == '\n') {
            if (++newlines <= 2) out.push_back('\n');
        } else {
            newlines = 0;
            out.push_back(c);
        }
    }
    size_t a = 0, b = out.size();
    while (a < b && (out[a] == '\n' || out[a] == ' ')) ++a;
    while (b > a && (out[b - 1] == '\n' || out[b - 1] == ' ')) --b;
    return out.substr(a, b - a);
}

inline std::string truncate(const std::string &s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen - 3) + "...";
}

// "Sep 12" for the current year, "Sep 12 2027" otherwise.
inline std::string formatDate(long long ms) {
    std::time_t t = (std::time_t)(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::time_t nowT = std::time(nullptr);
    std::tm nowTm{};
#ifdef _WIN32
    localtime_s(&nowTm, &nowT);
#else
    localtime_r(&nowT, &nowTm);
#endif
    char buf[32];
    if (tm.tm_year == nowTm.tm_year) std::strftime(buf, sizeof(buf), "%b %d", &tm);
    else std::strftime(buf, sizeof(buf), "%b %d %Y", &tm);
    return buf;
}

inline std::string formatClock(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    return buf;
}

inline long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace textutil
