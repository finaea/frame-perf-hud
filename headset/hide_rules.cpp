#include "hide_rules.h"

#include "atomic_file.h"
#include "json.h"

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <iterator>

bool globMatch(const std::string& pattern, const std::string& text) {
    // Iterative, with one backtrack point at the last '*' (enough for '*' and '?')
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool hideRuleMatches(const HideRule& rule, const std::string& comm, const std::string& exe, const std::string& cgroup) {
    if (rule.name.empty() && rule.cgroup.empty()) return false;
    if (!rule.name.empty() && !globMatch(rule.name, comm) && (exe.empty() || !globMatch(rule.name, exe))) return false;
    if (!rule.cgroup.empty() && (cgroup.empty() || !globMatch(rule.cgroup, cgroup))) return false;
    return true;
}

const std::vector<HideRule>& defaultHideRules() {
    // From `ps` + /proc/<pid>/cgroup on Jack's Frame with SteamVR and his own services running. Services under
    // system.slice and the desktop session's units are hidden by cgroup; Steam and SteamVR by name, because games run
    // in their cgroups. Jack's own units (eyecam, frameeyeosc, this HUD and its camera helper) aren't named.
    static const std::vector<HideRule> kRules = {
        {"", "/", "kernel threads"},
        {"", "/init.scope", "systemd"},
        {"", "/system.slice/*", "system services (NetworkManager, bluetooth, avahi, ...)"},
        {"", "*/user@*.service/init.scope", "the user's systemd"},
        {"", "*/app.slice/app-dbus*", "D-Bus activated helpers (accessibility, input method)"},
        {"", "*/app.slice/steamos-manager.service", "SteamOS manager"},
        {"", "*/app.slice/pidbridge.service", "SteamOS pidbridge"},
        {"", "*/app.slice/xdg-desktop-portal*", "desktop portals"},
        {"", "*/session.slice/gamescope-session.service", "gamescope + Xwayland"},
        {"", "*/session.slice/pipewire*", "audio"},
        {"", "*/session.slice/wireplumber.service", "audio"},
        {"", "*/session.slice/ibus-*", "input method"},
        {"", "*/session.slice/xdg-*", "desktop portals"},
        {"", "*/session.slice/plasma-*", "desktop portals"},
        {"", "*/session.slice/at-spi-dbus-bus.service", "accessibility bus"},
        {"", "*/session.slice/dbus-broker.service", "the session bus"},
        {"", "*/session.slice/steam-notif-daemon.service", "Steam notifications"},
        {"", "*/session.slice/steamos-powerbuttond.service", "power button"},
        {"", "*/session.slice/steamvr-*", "SteamVR helpers (logs, mic mute, cameras)"},
        {"sshd", "", "ssh"},
        {"sddm-helper", "", "login"},
        {"start-gamescope", "", "login"},
        {"steam", "", "Steam client"},
        {"steamwebhelper", "", "Steam client"},
        {"reaper", "", "Steam game launcher"},
        {"steam-launch-wrapper", "", "Steam game launcher"},
        {"steam-runtime-*", "", "Steam runtime"},
        {"pressure-vessel*", "", "Steam runtime"},
        {"pv-adverb", "", "Steam runtime"},
        {"srt-bwrap", "", "Steam runtime"},
        {"fossilize_replay", "", "Steam shader pre-caching"},
        {"run_vrlink.sh", "", "Steam Link streaming"},
        {"vrlink", "", "Steam Link streaming"},
        {"vrserver", "", "SteamVR server"},
        {"vrcompositor", "", "SteamVR compositor"},
        {"vrdashboard", "", "SteamVR dashboard"},
        {"vrwebhelper", "", "SteamVR dashboard"},
        {"vrmonitor", "", "SteamVR"},
        {"vrstartup", "", "SteamVR"},
        {"vrcmd", "", "SteamVR"},
        {"vrprismhost", "", "SteamVR"},
        {"XRService", "", "Steam Frame tracking"},
        {"eyetracking", "", "Steam Frame eye tracking"},
    };
    return kRules;
}

std::string hideRulesToJson(const std::vector<HideRule>& rules, uint32_t version) {
    std::string out = "{\n  \"version\": " + std::to_string(version) + ",\n  \"hide\": [\n";
    for (size_t i = 0; i < rules.size(); ++i) {
        const HideRule& r = rules[i];
        std::string line;
        if (!r.name.empty()) line += "\"name\": " + jsonString(r.name);
        if (!r.cgroup.empty()) line += (line.empty() ? "" : ", ") + std::string("\"cgroup\": ") + jsonString(r.cgroup);
        if (!r.note.empty()) line += (line.empty() ? "" : ", ") + std::string("\"note\": ") + jsonString(r.note);
        out += "    {" + line + "}" + (i + 1 < rules.size() ? "," : "") + "\n";
    }
    return out + "  ]\n}\n";
}

bool parseHideRules(const std::string& text, std::vector<HideRule>& out, std::string& error, uint32_t* version) {
    JsonValue root;
    if (!parseJson(text, root, error)) return false;
    const JsonValue* hide = root.isObject() ? root.get("hide") : nullptr;
    if (hide == nullptr || hide->type != JsonValue::Type::Array) {
        error = "expected { \"hide\": [ ... ] }";
        return false;
    }
    std::vector<HideRule> rules;
    for (size_t i = 0; i < hide->items.size(); ++i) {
        const JsonValue& item = hide->items[i];
        const std::string at = "hide[" + std::to_string(i) + "]";
        if (!item.isObject()) {
            error = at + " must be an object";
            return false;
        }
        HideRule rule;
        for (const auto& member : item.members) {
            std::string* field = member.first == "name" ? &rule.name
                                 : member.first == "cgroup" ? &rule.cgroup
                                 : member.first == "note" ? &rule.note
                                                          : nullptr;
            if (field == nullptr) continue;  // unknown keys are tolerated (a newer HUD may know more)
            if (!member.second.isString()) {
                error = at + "." + member.first + " must be a string";
                return false;
            }
            *field = member.second.text;
        }
        if (rule.name.empty() && rule.cgroup.empty()) {
            error = at + " needs \"name\" or \"cgroup\"";
            return false;
        }
        rules.push_back(std::move(rule));
    }
    out = std::move(rules);
    if (version != nullptr) {
        const JsonValue* v = root.get("version");
        // a file without one is from version 1 (the first to write the file)
        *version = v != nullptr && v->isNumber() && v->number >= 1 && v->number < 1e9 ? static_cast<uint32_t>(v->number)
                                                                                     : 1;
    }
    return true;
}

size_t addNewBuiltIns(std::vector<HideRule>& rules, uint32_t fileVersion, const std::vector<HideRule>& builtIns) {
    size_t added = 0;
    for (const HideRule& b : builtIns) {
        if (b.since <= fileVersion) continue;
        bool present = false;
        for (const HideRule& r : rules) present = present || (r.name == b.name && r.cgroup == b.cgroup);
        if (present) continue;
        rules.push_back(b);
        ++added;
    }
    return added;
}

std::string cgroupV2Path(const std::string& text) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        if (text.compare(start, 3, "0::") == 0) return text.substr(start + 3, end - start - 3);
        start = end + 1;
    }
    return "";
}

HideRules::HideRules(std::string path, double checkSeconds)
    : path_(std::move(path)), checkSeconds_(checkSeconds), rules_(defaultHideRules()) {}

bool HideRules::seedIfMissing(std::string& error, const std::vector<HideRule>& builtIns, uint32_t version) {
    struct stat st {};
    if (::stat(path_.c_str(), &st) != 0) {
        if (!fph::atomicWriteFile(path_, hideRulesToJson(builtIns, version), error)) return false;
        std::fprintf(stderr, "[hide] Wrote the default Hide system rules to %s\n", path_.c_str());
        return true;
    }
    // An older file gets the built-ins added since, once. One that doesn't parse is left for the user to fix
    std::ifstream file(path_, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<HideRule> rules;
    uint32_t fileVersion = 0;
    std::string parseError;
    if (!file || !parseHideRules(text, rules, parseError, &fileVersion) || fileVersion >= version) return true;
    const size_t added = addNewBuiltIns(rules, fileVersion, builtIns);
    if (!fph::atomicWriteFile(path_, hideRulesToJson(rules, version), error)) return true;  // the old file still works
    std::fprintf(stderr, "[hide] %s: version %u -> %u, %zu new built-in rule(s) added\n", path_.c_str(), fileVersion,
                 version, added);
    return true;
}

bool HideRules::refresh(double now) {
    if (now - lastCheck_ < checkSeconds_) return false;
    lastCheck_ = now;
    struct stat st {};
    if (::stat(path_.c_str(), &st) != 0) {
        if (seen_) std::fprintf(stderr, "[hide] %s is gone: keeping the rules in use\n", path_.c_str());
        seen_ = false;
        broken_ = false;
        return false;
    }
    const int64_t mtime = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
    if (seen_ && mtime == mtimeNs_ && static_cast<int64_t>(st.st_size) == size_) return false;
    seen_ = true;
    mtimeNs_ = mtime;
    size_ = static_cast<int64_t>(st.st_size);
    std::ifstream file(path_, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<HideRule> rules;
    std::string error;
    if (!file || !parseHideRules(text, rules, error)) {
        std::fprintf(stderr, "[hide] Can't use %s (%s): keeping the last good rules\n", path_.c_str(),
                     error.empty() ? "can't read it" : error.c_str());
        broken_ = true;
        return false;
    }
    broken_ = false;
    if (rules.size() == rules_.size()) {
        bool same = true;
        for (size_t i = 0; i < rules.size() && same; ++i)
            same = rules[i].name == rules_[i].name && rules[i].cgroup == rules_[i].cgroup;
        if (same) return false;
    }
    rules_ = std::move(rules);
    ++generation_;
    std::fprintf(stderr, "[hide] Loaded %zu Hide system rules from %s\n", rules_.size(), path_.c_str());
    return true;
}

bool HideRules::hides(const std::string& comm, const std::string& exe, const std::string& cgroup) const {
    for (const HideRule& rule : rules_) {
        if (hideRuleMatches(rule, comm, exe, cgroup)) return true;
    }
    return false;
}
