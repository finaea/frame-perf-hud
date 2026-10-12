#include "settings.h"

#include "atomic_file.h"
#include "json.h"
#include "link_wire.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include "win_text.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fph {
namespace pc {

bool Settings::operator==(const Settings& o) const {
    if (pcId != o.pcId || manualIp != o.manualIp || onTop != o.onTop || trayHintShown != o.trayHintShown ||
        cameraFps != o.cameraFps) {
        return false;
    }
    if (window.valid != o.window.valid || window.x != o.window.x || window.y != o.window.y ||
        window.width != o.window.width || window.height != o.window.height || window.maximized != o.window.maximized) {
        return false;
    }
    if (headsets.size() != o.headsets.size()) return false;
    for (size_t i = 0; i < headsets.size(); ++i) {
        if (headsets[i].id != o.headsets[i].id || headsets[i].name != o.headsets[i].name ||
            headsets[i].ips != o.headsets[i].ips) {
            return false;
        }
    }
    return true;
}

namespace {

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

const char* boolText(bool b) {
    return b ? "true" : "false";
}

bool readInt(const JsonValue& obj, const char* key, int& out, std::vector<std::string>& warnings) {
    const JsonValue* v = obj.get(key);
    if (v == nullptr) return false;
    if (!v->isNumber() || !std::isfinite(v->number) || std::fabs(v->number) > 1e8) {
        warnings.push_back(std::string("settings: \"") + key + "\" isn't a number");
        return false;
    }
    out = static_cast<int>(v->number);
    return true;
}

bool readBool(const JsonValue& obj, const char* key, bool& out, std::vector<std::string>& warnings) {
    const JsonValue* v = obj.get(key);
    if (v == nullptr) return false;
    if (!v->isBool()) {
        warnings.push_back(std::string("settings: \"") + key + "\" isn't true/false");
        return false;
    }
    out = v->boolean;
    return true;
}

bool readString(const JsonValue& obj, const char* key, std::string& out, std::vector<std::string>& warnings) {
    const JsonValue* v = obj.get(key);
    if (v == nullptr) return false;
    if (!v->isString()) {
        warnings.push_back(std::string("settings: \"") + key + "\" isn't a string");
        return false;
    }
    out = v->text;
    return true;
}

bool validIp(const std::string& text) {
    uint32_t ip = 0;
    return link::ipv4FromText(text, ip);
}

bool readFile(const std::string& path, std::string& out, bool& exists) {
#ifdef _WIN32
    std::ifstream in(widen(path).c_str(), std::ios::binary);
#else
    std::ifstream in(path, std::ios::binary);
#endif
    exists = static_cast<bool>(in);
    if (!in) return false;
    std::stringstream text;
    text << in.rdbuf();
    out = text.str();
    return true;
}

void moveAside(const std::string& path) {
    const std::string target = path + ".broken";
#ifdef _WIN32
    MoveFileExW(widen(path).c_str(), widen(target).c_str(), MOVEFILE_REPLACE_EXISTING);
#else
    std::rename(path.c_str(), target.c_str());
#endif
}

}  // namespace

std::string settingsToJson(const Settings& s) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"pc_id\": " << quote(s.pcId) << ",\n";
    o << "  \"manual_ip\": " << quote(s.manualIp) << ",\n";
    o << "  \"on_top\": " << boolText(s.onTop) << ",\n";
    o << "  \"tray_hint_shown\": " << boolText(s.trayHintShown) << ",\n";
    o << "  \"camera_fps\": " << s.cameraFps << ",\n";
    o << "  \"window\": {\"valid\": " << boolText(s.window.valid) << ", \"x\": " << s.window.x
      << ", \"y\": " << s.window.y << ", \"width\": " << s.window.width << ", \"height\": " << s.window.height
      << ", \"maximized\": " << boolText(s.window.maximized) << "},\n";
    o << "  \"headsets\": [";
    for (size_t i = 0; i < s.headsets.size(); ++i) {
        const RememberedHeadset& h = s.headsets[i];
        o << (i == 0 ? "\n" : ",\n") << "    {\"id\": " << quote(h.id) << ", \"name\": " << quote(h.name)
          << ", \"ips\": [";
        for (size_t j = 0; j < h.ips.size(); ++j) o << (j == 0 ? "" : ", ") << quote(h.ips[j]);
        o << "]}";
    }
    o << (s.headsets.empty() ? "]\n" : "\n  ]\n");
    o << "}\n";
    return o.str();
}

bool settingsFromJson(const std::string& text, Settings& out, std::vector<std::string>& warnings, std::string& error) {
    JsonValue root;
    if (!parseJson(text, root, error)) return false;
    if (!root.isObject()) {
        error = "not a JSON object";
        return false;
    }
    Settings s;
    std::string id;
    link::Id128 parsed {};
    if (readString(root, "pc_id", id, warnings)) {
        if (link::idFromHex(id, parsed)) s.pcId = id;
        else warnings.push_back("settings: \"pc_id\" isn't 32 hex chars");
    }
    std::string ip;
    if (readString(root, "manual_ip", ip, warnings)) {
        if (ip.empty() || validIp(ip)) s.manualIp = ip;
        else warnings.push_back("settings: \"manual_ip\" isn't an IPv4 address");
    }
    readBool(root, "on_top", s.onTop, warnings);
    readBool(root, "tray_hint_shown", s.trayHintShown, warnings);
    if (readInt(root, "camera_fps", s.cameraFps, warnings)) s.cameraFps = std::clamp(s.cameraFps, 1, 30);
    if (const JsonValue* w = root.get("window"); w != nullptr && w->isObject()) {
        WindowState ws;
        readBool(*w, "valid", ws.valid, warnings);
        readInt(*w, "x", ws.x, warnings);
        readInt(*w, "y", ws.y, warnings);
        readInt(*w, "width", ws.width, warnings);
        readInt(*w, "height", ws.height, warnings);
        readBool(*w, "maximized", ws.maximized, warnings);
        if (ws.width < 100 || ws.height < 100) ws.valid = false;
        s.window = ws;
    }
    if (const JsonValue* list = root.get("headsets"); list != nullptr && list->type == JsonValue::Type::Array) {
        for (const JsonValue& item : list->items) {
            if (!item.isObject() || s.headsets.size() >= kMaxRememberedHeadsets) continue;
            RememberedHeadset h;
            if (!readString(item, "id", h.id, warnings) || !link::idFromHex(h.id, parsed)) continue;
            readString(item, "name", h.name, warnings);
            if (const JsonValue* ips = item.get("ips"); ips != nullptr && ips->type == JsonValue::Type::Array) {
                for (const JsonValue& v : ips->items) {
                    if (v.isString() && validIp(v.text) && h.ips.size() < kMaxIpsPerHeadset) h.ips.push_back(v.text);
                }
            }
            s.headsets.push_back(h);
        }
    }
    out = s;
    return true;
}

LoadResult loadSettings(const std::string& path, Settings& out, bool& idCreated, std::vector<std::string>& warnings) {
    idCreated = false;
    out = Settings();
    std::string text;
    bool exists = false;
    LoadResult result = LoadResult::Missing;
    if (readFile(path, text, exists)) {
        std::string error;
        if (settingsFromJson(text, out, warnings, error)) {
            result = LoadResult::Loaded;
        } else {
            warnings.push_back("settings: " + error + "; starting over (old file kept as settings.json.broken)");
            moveAside(path);
            out = Settings();
            result = LoadResult::Broken;
        }
    }
    if (out.pcId.empty()) {
        out.pcId = link::idToHex(link::randomId());
        idCreated = true;
    }
    return result;
}

bool saveSettings(const std::string& path, const Settings& s, std::string& error) {
    return atomicWriteFile(path, settingsToJson(s), error);
}

bool rememberHeadset(Settings& s, const std::string& id, const std::string& name, const std::vector<std::string>& ips) {
    RememberedHeadset h;
    h.id = id;
    h.name = name;
    for (const std::string& ip : ips) {
        if (validIp(ip) && std::find(h.ips.begin(), h.ips.end(), ip) == h.ips.end()) h.ips.push_back(ip);
    }
    auto it = std::find_if(s.headsets.begin(), s.headsets.end(), [&](const RememberedHeadset& r) { return r.id == id; });
    if (it != s.headsets.end()) {
        // Keep older IPs after the new ones (the other path may come back)
        for (const std::string& ip : it->ips) {
            if (std::find(h.ips.begin(), h.ips.end(), ip) == h.ips.end()) h.ips.push_back(ip);
        }
    }
    if (h.ips.size() > kMaxIpsPerHeadset) h.ips.resize(kMaxIpsPerHeadset);
    if (it != s.headsets.end() && it == s.headsets.begin() && it->name == h.name && it->ips == h.ips) return false;
    if (it != s.headsets.end()) s.headsets.erase(it);
    s.headsets.insert(s.headsets.begin(), h);
    if (s.headsets.size() > kMaxRememberedHeadsets) s.headsets.resize(kMaxRememberedHeadsets);
    return true;
}

std::vector<std::string> rememberedIps(const Settings& s) {
    std::vector<std::string> out;
    for (const RememberedHeadset& h : s.headsets) {
        for (const std::string& ip : h.ips) {
            if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
        }
    }
    return out;
}

}  // namespace pc
}  // namespace fph
