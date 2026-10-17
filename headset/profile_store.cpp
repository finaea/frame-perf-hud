#include "profile_store.h"

#include "atomic_file.h"
#include "json.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

constexpr const char* kPrefix = "profile-";

/** @return N of "profile-N", 0 if the id isn't one */
int idNumber(const std::string& id) {
    if (id.rfind(kPrefix, 0) != 0 || id.size() == 8 || id.size() > 10) return 0;
    int n = 0;
    for (size_t i = 8; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9') return 0;
        n = n * 10 + (id[i] - '0');
    }
    return n >= 1 && n <= static_cast<int>(ProfileStore::kMaxProfiles) ? n : 0;
}

}  // namespace

ProfileStore::ProfileStore(std::string dir) : dir_(std::move(dir)) {}

std::string ProfileStore::pathOf(const std::string& id) const {
    return dir_ + "/" + id + ".json";
}

std::string ProfileStore::cleanName(const std::string& name) {
    const size_t a = name.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::string out = name.substr(a, name.find_last_not_of(" \t\r\n") - a + 1);
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';  // a pasted newline or tab
    }
    if (out.size() > kMaxNameBytes) {
        size_t cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;  // not inside a character
        out.resize(cut);
    }
    return out;
}

std::string ProfileStore::toJson(const Profile& profile) {
    return "{\n  \"name\": " + jsonString(profile.name) + ",\n  \"layout\": " + fph::layoutToJson(profile.layout, 2) +
           "\n}\n";
}

bool ProfileStore::fromJson(const std::string& text, Profile& out, std::vector<std::string>& warnings,
                            std::string& error) {
    JsonValue root;
    if (!parseJson(text, root, error)) return false;
    const JsonValue* name = root.get("name");
    const JsonValue* layout = root.get("layout");
    if (layout == nullptr) {
        error = "no \"layout\"";
        return false;
    }
    Profile p;
    if (!fph::layoutFromJson(*layout, p.layout, warnings, error)) return false;
    p.name = name != nullptr && name->isString() ? cleanName(name->text) : "";
    out.name = p.name;
    out.layout = p.layout;
    return true;
}

void ProfileStore::load(std::vector<std::string>& warnings) {
    profiles_.clear();
    for (size_t n = 1; n <= kMaxProfiles; ++n) {
        Profile p;
        p.id = kPrefix + std::to_string(n);
        std::ifstream file(pathOf(p.id), std::ios::binary);
        if (!file) continue;
        std::stringstream text;
        text << file.rdbuf();
        std::string error;
        std::vector<std::string> notes;
        if (!fromJson(text.str(), p, notes, error)) {
            warnings.push_back(pathOf(p.id) + ": " + error + " (skipped)");
            continue;
        }
        for (const std::string& w : notes) warnings.push_back(pathOf(p.id) + ": " + w);
        if (p.name.empty()) p.name = "Profile " + std::to_string(n);
        profiles_.push_back(p);
    }
    ++revision_;
}

const Profile* ProfileStore::find(const std::string& id) const {
    for (const Profile& p : profiles_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

bool ProfileStore::unsaved(const fph::Layout& live, const std::string& activeId) const {
    if (const Profile* active = find(activeId)) return !live.sameWidgets(active->layout);
    return std::none_of(profiles_.begin(), profiles_.end(), [&](const Profile& p) { return live.sameWidgets(p.layout); });
}

Profile* ProfileStore::findMutable(const std::string& id) {
    for (Profile& p : profiles_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

bool ProfileStore::write(const Profile& profile, std::string& error) {
    return fph::atomicWriteFile(pathOf(profile.id), toJson(profile), error);
}

bool ProfileStore::create(const fph::Layout& layout, std::string& id, std::string& error) {
    if (!canCreate()) {
        error = "all " + std::to_string(kMaxProfiles) + " profiles are used";
        return false;
    }
    size_t n = 1;
    while (n <= kMaxProfiles && find(kPrefix + std::to_string(n)) != nullptr) ++n;
    Profile p;
    p.id = kPrefix + std::to_string(n);
    p.name = "Profile " + std::to_string(n);
    p.layout = layout;
    if (!write(p, error)) return false;
    profiles_.push_back(p);
    std::sort(profiles_.begin(), profiles_.end(),
              [](const Profile& a, const Profile& b) { return idNumber(a.id) < idNumber(b.id); });
    id = p.id;
    ++revision_;
    return true;
}

bool ProfileStore::save(const std::string& id, const fph::Layout& layout, std::string& error) {
    Profile* p = findMutable(id);
    if (p == nullptr) {
        error = "no profile " + id;
        return false;
    }
    Profile next = *p;
    next.layout = layout;
    if (!write(next, error)) return false;
    *p = next;
    ++revision_;
    return true;
}

bool ProfileStore::rename(const std::string& id, const std::string& name, std::string& error) {
    Profile* p = findMutable(id);
    if (p == nullptr) {
        error = "no profile " + id;
        return false;
    }
    const std::string clean = cleanName(name);
    if (clean.empty() || clean == p->name) return true;
    Profile next = *p;
    next.name = clean;
    if (!write(next, error)) return false;
    *p = next;
    ++revision_;
    return true;
}

bool ProfileStore::remove(const std::string& id, std::string& error) {
    const auto it = std::find_if(profiles_.begin(), profiles_.end(), [&](const Profile& p) { return p.id == id; });
    if (it == profiles_.end()) {
        error = "no profile " + id;
        return false;
    }
    if (std::remove(pathOf(id).c_str()) != 0) {
        std::ifstream still(pathOf(id));
        if (still) {
            error = "can't delete " + pathOf(id);
            return false;
        }
    }
    profiles_.erase(it);
    ++revision_;
    return true;
}
