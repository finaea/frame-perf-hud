// 設定の読み込みと変更の監視の実装。
#include "config.h"

#include "atomic_file.h"  // T04
#include "json.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

/**
 * オブジェクトのメンバーのうち、知っているキー以外を warnings に入れる。
 * @param object 調べるオブジェクト
 * @param known 知っているキーの一覧
 * @param prefix 表示用のキーの前置き（"thresholds." など）
 * @param warnings 追加先
 */
void warnUnknownKeys(const JsonValue& object, const std::vector<std::string>& known, const std::string& prefix,
                     std::vector<std::string>& warnings) {
    for (const auto& member : object.members) {
        if (std::find(known.begin(), known.end(), member.first) == known.end()) {
            warnings.push_back("unknown key: " + prefix + member.first);
        }
    }
}

/**
 * 数値のキーを読んで範囲に収める。キーが無ければ何もしない。
 * @param object 読むオブジェクト
 * @param key キー
 * @param minValue 最小値
 * @param maxValue 最大値
 * @param target 書き込み先
 * @param warnings 型違い・範囲外のときの追加先
 */
void readNumber(const JsonValue& object, const char* key, double minValue, double maxValue, double& target,
                std::vector<std::string>& warnings) {
    const JsonValue* value = object.get(key);
    if (value == nullptr) return;
    if (!value->isNumber()) {
        warnings.push_back(std::string(key) + " must be a number");
        return;
    }
    const double clamped = std::clamp(value->number, minValue, maxValue);
    if (clamped != value->number) {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", clamped);
        warnings.push_back(std::string(key) + " is out of range, clamped to " + text);
    }
    target = clamped;
}

/**
 * 整数のキーを読んで範囲に収める。キーが無ければ何もしない。
 * @param object 読むオブジェクト
 * @param key キー
 * @param minValue 最小値
 * @param maxValue 最大値
 * @param target 書き込み先
 * @param warnings 型違い・範囲外のときの追加先
 */
void readInt(const JsonValue& object, const char* key, int minValue, int maxValue, int& target,
             std::vector<std::string>& warnings) {
    double value = target;
    readNumber(object, key, minValue, maxValue, value, warnings);
    target = static_cast<int>(value);
}

/**
 * 真偽値のキーを読む。キーが無ければ何もしない。
 * @param object 読むオブジェクト
 * @param key キー
 * @param target 書き込み先
 * @param warnings 型違いのときの追加先
 */
void readBool(const JsonValue& object, const char* key, bool& target, std::vector<std::string>& warnings) {
    const JsonValue* value = object.get(key);
    if (value == nullptr) return;
    if (!value->isBool()) {
        warnings.push_back(std::string(key) + " must be true or false");
        return;
    }
    target = value->boolean;
}

/**
 * 文字列のキーを読む。キーが無ければ何もしない。
 * @param object 読むオブジェクト
 * @param key キー
 * @param target 書き込み先
 * @param warnings 型違いのときの追加先
 */
void readString(const JsonValue& object, const char* key, std::string& target, std::vector<std::string>& warnings) {
    const JsonValue* value = object.get(key);
    if (value == nullptr) return;
    if (!value->isString()) {
        warnings.push_back(std::string(key) + " must be a string");
        return;
    }
    target = value->text;
}

/**
 * thresholds オブジェクトを読む。
 * @param object thresholds の中身
 * @param th 書き込み先
 * @param warnings 追加先
 */
void readThresholds(const JsonValue& object, Thresholds& th, std::vector<std::string>& warnings) {
    warnUnknownKeys(object,
                    {"fps_warn_ratio", "fps_crit_ratio", "frame_warn_ratio", "frame_crit_ratio", "reproj_warn_pct", "reproj_crit_pct", "temp_warn_c",
                     "temp_crit_c", "battery_warn_pct", "battery_crit_pct", "power_warn_w", "power_crit_w",
                     "cpu_warn_pct", "cpu_crit_pct", "gpu_warn_pct", "gpu_crit_pct", "wifi_warn_dbm", "wifi_crit_dbm",
                     "controller_warn_pct", "controller_crit_pct"},
                    "thresholds.", warnings);
    readNumber(object, "fps_warn_ratio", 0.0, 1.0, th.fpsWarnRatio, warnings);
    readNumber(object, "fps_crit_ratio", 0.0, 1.0, th.fpsCritRatio, warnings);
    readNumber(object, "frame_warn_ratio", 0.1, 10.0, th.frameWarnRatio, warnings);
    readNumber(object, "frame_crit_ratio", 0.1, 10.0, th.frameCritRatio, warnings);
    readNumber(object, "reproj_warn_pct", 0.0, 100.0, th.reprojWarnPct, warnings);
    readNumber(object, "reproj_crit_pct", 0.0, 100.0, th.reprojCritPct, warnings);
    readNumber(object, "temp_warn_c", 0.0, 150.0, th.tempWarnC, warnings);
    readNumber(object, "temp_crit_c", 0.0, 150.0, th.tempCritC, warnings);
    readNumber(object, "battery_warn_pct", 0.0, 100.0, th.batteryWarnPct, warnings);
    readNumber(object, "battery_crit_pct", 0.0, 100.0, th.batteryCritPct, warnings);
    readNumber(object, "power_warn_w", 0.0, 100.0, th.powerWarnW, warnings);
    readNumber(object, "power_crit_w", 0.0, 100.0, th.powerCritW, warnings);
    readNumber(object, "cpu_warn_pct", 0.0, 100.0, th.cpuWarnPct, warnings);
    readNumber(object, "cpu_crit_pct", 0.0, 100.0, th.cpuCritPct, warnings);
    readNumber(object, "gpu_warn_pct", 0.0, 100.0, th.gpuWarnPct, warnings);
    readNumber(object, "gpu_crit_pct", 0.0, 100.0, th.gpuCritPct, warnings);
    readNumber(object, "wifi_warn_dbm", -120.0, 0.0, th.wifiWarnDbm, warnings);
    readNumber(object, "wifi_crit_dbm", -120.0, 0.0, th.wifiCritDbm, warnings);
    readNumber(object, "controller_warn_pct", 0.0, 100.0, th.controllerWarnPct, warnings);
    readNumber(object, "controller_crit_pct", 0.0, 100.0, th.controllerCritPct, warnings);
}

/**
 * 手首の位置と向き（left_wrist / right_wrist）を読む。キーが無ければ何もしない。
 * @param root 設定ファイルの一番外側
 * @param key "left_wrist" か "right_wrist"
 * @param pose 書き込み先
 * @param warnings 追加先
 */
void readWristPose(const JsonValue& root, const char* key, WristPose& pose, std::vector<std::string>& warnings) {
    const JsonValue* object = root.get(key);
    if (object == nullptr) return;
    if (!object->isObject()) {
        warnings.push_back(std::string(key) + " must be {\"x\":..,\"y\":..,\"z\":..,\"pitch\":..,\"yaw\":..,\"roll\":..}");
        return;
    }
    warnUnknownKeys(*object, {"x", "y", "z", "pitch", "yaw", "roll"}, std::string(key) + ".", warnings);
    readNumber(*object, "x", -0.5, 0.5, pose.x, warnings);
    readNumber(*object, "y", -0.5, 0.5, pose.y, warnings);
    readNumber(*object, "z", -0.5, 0.5, pose.z, warnings);
    readNumber(*object, "pitch", -180.0, 0.0, pose.pitch, warnings);
    readNumber(*object, "yaw", -180.0, 180.0, pose.yaw, warnings);
    readNumber(*object, "roll", -180.0, 180.0, pose.roll, warnings);
}

}  // namespace

const char* attachmentName(Attachment attachment) {
    switch (attachment) {
        case Attachment::LeftWrist: return "left_wrist";
        case Attachment::RightWrist: return "right_wrist";
        case Attachment::Detached: return "detached";  // T05
        case Attachment::Head: break;
    }
    return "head";
}

WristPose& selectedWrist(Config& config) {
    return config.attachment == Attachment::RightWrist ? config.rightWrist : config.leftWrist;
}

const WristPose& selectedWrist(const Config& config) {
    return config.attachment == Attachment::RightWrist ? config.rightWrist : config.leftWrist;
}

std::string defaultConfigPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg != nullptr && xdg[0] != '\0') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = std::string(home != nullptr ? home : ".") + "/.config";
    }
    return base + "/frame-perf-hud/config.json";
}

bool loadConfig(const std::string& path, Config& out, std::vector<std::string>& warnings, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        out = Config();  // ファイルが無いときは既定値
        return true;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();

    JsonValue root;
    if (!parseJson(buffer.str(), root, error)) return false;
    if (!root.isObject()) {
        error = "the top level must be { ... }";
        return false;
    }

    Config config;  // 書かれていない項目は既定値
    warnUnknownKeys(root,
                    {"visible", "update_check", "language", "attachment", "position", "rotation", "left_wrist",
                     "right_wrist", "wrist_fade", "wrist_fade_end_deg", "clock_format", "width_m", "alpha",
                     "update_interval_ms", "graph_seconds", "font", "font_bold", "thresholds", "layout",
                     "detached_pose", "link", "crash_recorder", "active_profile", "detached_curve_deg",
                     "detached_curve"},  // T05, T06, T11, R2, R3 (R2's detached_curve: ignored, gone on the next save)
                    "", warnings);
    readBool(root, "visible", config.visible, warnings);
    readBool(root, "update_check", config.updateCheck, warnings);
    readBool(root, "crash_recorder", config.crashRecorder, warnings);  // T11
    readString(root, "active_profile", config.activeProfile, warnings);  // R2
    std::string language = languageCode(config.language);
    readString(root, "language", language, warnings);
    if (!parseLanguage(language, config.language)) warnings.push_back("language must be \"en\"");
    std::string attachment = attachmentName(config.attachment);
    readString(root, "attachment", attachment, warnings);
    if (attachment == "left_wrist") {
        config.attachment = Attachment::LeftWrist;
    } else if (attachment == "right_wrist") {
        config.attachment = Attachment::RightWrist;
    } else if (attachment == "detached") {  // T05
        config.attachment = Attachment::Detached;
    } else if (attachment != "head") {
        warnings.push_back("attachment must be \"head\", \"left_wrist\", \"right_wrist\" or \"detached\"");
    }
    // T05: the room pose, 12 numbers (3×4, row-major). Anything else is ignored and the HUD is re-placed in front
    if (const JsonValue* pose = root.get("detached_pose")) {
        bool ok = pose->type == JsonValue::Type::Array && pose->items.size() == 12;
        for (size_t i = 0; ok && i < 12; ++i) ok = pose->items[i].isNumber() && std::isfinite(pose->items[i].number);
        if (ok) {
            for (size_t i = 0; i < 12; ++i) config.detachedPose.m[i / 4][i % 4] = pose->items[i].number;
            config.detachedPose.valid = true;
        } else {
            warnings.push_back("detached_pose must be 12 numbers (a 3x4 matrix, row by row); it will be placed again");
        }
    }
    // R3: the arc the Detached HUD wraps, 0..90°, rounded to a 10° step (R2's 0-3 "detached_curve" is gone)
    double curveDeg = config.detachedCurveDeg;
    readNumber(root, "detached_curve_deg", 0.0, 90.0, curveDeg, warnings);
    config.detachedCurveDeg = static_cast<int>(std::lround(curveDeg / 10.0)) * 10;
    if (const JsonValue* position = root.get("position")) {
        if (position->isObject()) {
            warnUnknownKeys(*position, {"x", "y", "z"}, "position.", warnings);
            readNumber(*position, "x", -5.0, 5.0, config.posX, warnings);
            readNumber(*position, "y", -5.0, 5.0, config.posY, warnings);
            readNumber(*position, "z", -5.0, 5.0, config.posZ, warnings);
        } else {
            warnings.push_back("position must be {\"x\":..,\"y\":..,\"z\":..}");
        }
    }
    if (const JsonValue* rotation = root.get("rotation")) {
        if (rotation->isObject()) {
            warnUnknownKeys(*rotation, {"yaw", "pitch", "roll"}, "rotation.", warnings);
            readNumber(*rotation, "yaw", -180.0, 180.0, config.yawDeg, warnings);
            readNumber(*rotation, "pitch", -90.0, 90.0, config.pitchDeg, warnings);
            readNumber(*rotation, "roll", -180.0, 180.0, config.rollDeg, warnings);
        } else {
            warnings.push_back("rotation must be {\"yaw\":..,\"pitch\":..,\"roll\":..} (degrees)");
        }
    }
    readWristPose(root, "left_wrist", config.leftWrist, warnings);
    readWristPose(root, "right_wrist", config.rightWrist, warnings);
    readBool(root, "wrist_fade", config.wristFade, warnings);
    readNumber(root, "wrist_fade_end_deg", 35.0, 90.0, config.wristFadeEndDeg, warnings);
    if (const JsonValue* clock = root.get("clock_format")) {
        if (clock->isNumber() && (clock->number == 0 || clock->number == 12 || clock->number == 24)) {
            config.clockFormat = static_cast<int>(clock->number);
        } else {
            warnings.push_back("clock_format must be 0 (hidden), 12 or 24");
        }
    }
    readNumber(root, "width_m", 0.03, 2.0, config.widthM, warnings);
    readNumber(root, "alpha", 0.0, 1.0, config.alpha, warnings);
    readInt(root, "update_interval_ms", 100, 5000, config.updateIntervalMs, warnings);
    readInt(root, "graph_seconds", 5, 300, config.graphSeconds, warnings);
    readString(root, "font", config.fontPath, warnings);
    readString(root, "font_bold", config.boldFontPath, warnings);
    if (const JsonValue* thresholds = root.get("thresholds")) {
        if (thresholds->isObject()) {
            readThresholds(*thresholds, config.thresholds, warnings);
        } else {
            warnings.push_back("thresholds must be { ... }");
        }
    }
    // T04: the widget grid. A broken layout falls back to the default one; the rest of the file still counts
    if (const JsonValue* layout = root.get("layout")) {
        std::string layoutError;
        if (fph::layoutFromJson(*layout, config.layout, warnings, layoutError)) {
            config.layoutInFile = true;
        } else {
            warnings.push_back(layoutError + ": using the default layout");
        }
    }
    // T06: the PC link. A bad entry is skipped with a warning; the rest still counts
    if (const JsonValue* link = root.get("link")) {
        if (link->isObject()) {
            warnUnknownKeys(*link, {"port", "headset_id", "allowed_pcs"}, "link.", warnings);
            readInt(*link, "port", 1024, 65535, config.link.port, warnings);
            readString(*link, "headset_id", config.link.headsetId, warnings);
            const auto isHexId = [](const std::string& s) {
                return s.size() == 32 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
            };
            if (!config.link.headsetId.empty() && !isHexId(config.link.headsetId)) {
                warnings.push_back("link.headset_id must be 32 lowercase hex chars; a new one will be made");
                config.link.headsetId.clear();
            }
            if (const JsonValue* pcs = link->get("allowed_pcs")) {
                if (pcs->type == JsonValue::Type::Array) {
                    for (const JsonValue& pc : pcs->items) {
                        LinkConfig::AllowedPc entry;
                        const JsonValue* id = pc.get("id");
                        const JsonValue* name = pc.get("name");
                        if (id == nullptr || !id->isString() || !isHexId(id->text)) {
                            warnings.push_back("link.allowed_pcs: an entry without a valid \"id\" was skipped");
                            continue;
                        }
                        entry.id = id->text;
                        if (name != nullptr && name->isString()) entry.name = name->text;
                        config.link.allowedPcs.push_back(entry);
                    }
                } else {
                    warnings.push_back("link.allowed_pcs must be [ {\"id\": ..., \"name\": ...}, ... ]");
                }
            }
        } else {
            warnings.push_back("link must be { ... }");
        }
    }
    out = config;
    return true;
}

namespace {

/**
 * 数値を JSON 用の短い文字列にする。
 * @param value 値
 * @return 文字列（例: 0.15、-0.5、500）
 */
std::string jsonNumber(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.4g", value);
    return text;
}

/** T06: the "link" object (with its trailing comma). PC names come off the network: control chars are dropped. */
std::string linkJson(const LinkConfig& link) {
    const auto quote = [](const std::string& text) {
        std::string out = "\"";
        for (const char c : text) {
            if (static_cast<unsigned char>(c) < 0x20) continue;
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out + "\"";
    };
    std::string out = "  \"link\": {\n    \"port\": " + std::to_string(link.port) + ",\n    \"headset_id\": " +
                      quote(link.headsetId) + ",\n    \"allowed_pcs\": [";
    for (size_t i = 0; i < link.allowedPcs.size(); ++i) {
        out += std::string(i ? "," : "") + "\n      { \"id\": " + quote(link.allowedPcs[i].id) +
               ", \"name\": " + quote(link.allowedPcs[i].name) + " }";
    }
    return out + (link.allowedPcs.empty() ? "]\n  },\n" : "\n    ]\n  },\n");
}

/** T05: the "detached_pose" line (with its trailing comma), or "" when the HUD was never placed in the room. */
std::string detachedJson(const DetachedPose& pose) {
    if (!pose.valid) return "";
    std::string out = "  \"detached_pose\": [";
    for (int i = 0; i < 12; ++i) {
        char text[32];
        // 6 digits: a rotation saved with 4 would drift visibly from orthonormal over a few saves
        std::snprintf(text, sizeof(text), "%s%.6g", i ? ", " : "", pose.m[i / 4][i % 4] + 0.0);
        out += text;
    }
    return out + "],\n";
}

}  // namespace

bool saveConfig(const std::string& path, const Config& config, std::string& error) {
    /**
     * 手首の位置と向きを 1 行の JSON にする。
     */
    const auto poseJson = [](const WristPose& p) {
        return "{ \"x\": " + jsonNumber(p.x) + ", \"y\": " + jsonNumber(p.y) + ", \"z\": " + jsonNumber(p.z) +
               ", \"pitch\": " + jsonNumber(p.pitch) + ", \"yaw\": " + jsonNumber(p.yaw) +
               ", \"roll\": " + jsonNumber(p.roll) + " }";
    };
    const Thresholds& th = config.thresholds;
    std::ostringstream out;
    out << "{\n"
        << "  \"visible\": " << (config.visible ? "true" : "false") << ",\n"
        << "  \"update_check\": " << (config.updateCheck ? "true" : "false") << ",\n"
        << "  \"crash_recorder\": " << (config.crashRecorder ? "true" : "false") << ",\n"  // T11
        << "  \"active_profile\": " << jsonString(config.activeProfile) << ",\n"            // R2
        << "  \"language\":\"" << languageCode(config.language) << "\",\n"
        << "  \"attachment\": " << jsonString(attachmentName(config.attachment)) << ",\n"
        << "  \"position\": { \"x\": " << jsonNumber(config.posX) << ", \"y\": " << jsonNumber(config.posY)
        << ", \"z\": " << jsonNumber(config.posZ) << " },\n"
        << "  \"rotation\": { \"yaw\": " << jsonNumber(config.yawDeg) << ", \"pitch\": " << jsonNumber(config.pitchDeg)
        << ", \"roll\": " << jsonNumber(config.rollDeg) << " },\n"
        << "  \"left_wrist\": " << poseJson(config.leftWrist) << ",\n"
        << "  \"right_wrist\": " << poseJson(config.rightWrist) << ",\n"
        << detachedJson(config.detachedPose)  // T05: "" until placed
        << "  \"detached_curve_deg\": " << config.detachedCurveDeg << ",\n"  // R3
        << "  \"wrist_fade\": " << (config.wristFade ? "true" : "false") << ",\n"
        << "  \"wrist_fade_end_deg\": " << jsonNumber(config.wristFadeEndDeg) << ",\n"
        << "  \"clock_format\": " << config.clockFormat << ",\n"
        << "  \"width_m\": " << jsonNumber(config.widthM) << ",\n"
        << "  \"alpha\": " << jsonNumber(config.alpha) << ",\n"
        << "  \"update_interval_ms\": " << config.updateIntervalMs << ",\n"
        << "  \"graph_seconds\": " << config.graphSeconds << ",\n"
        << "  \"font\": " << jsonString(config.fontPath) << ",\n"
        << "  \"font_bold\": " << jsonString(config.boldFontPath) << ",\n"
        << "  \"thresholds\": {\n"
        << "    \"fps_warn_ratio\": " << jsonNumber(th.fpsWarnRatio) << ",\n"
        << "    \"fps_crit_ratio\": " << jsonNumber(th.fpsCritRatio) << ",\n"
        << "    \"frame_warn_ratio\": " << jsonNumber(th.frameWarnRatio) << ",\n"
        << "    \"frame_crit_ratio\": " << jsonNumber(th.frameCritRatio) << ",\n"
        << "    \"reproj_warn_pct\": " << jsonNumber(th.reprojWarnPct) << ",\n"
        << "    \"reproj_crit_pct\": " << jsonNumber(th.reprojCritPct) << ",\n"
        << "    \"temp_warn_c\": " << jsonNumber(th.tempWarnC) << ",\n"
        << "    \"temp_crit_c\": " << jsonNumber(th.tempCritC) << ",\n"
        << "    \"battery_warn_pct\": " << jsonNumber(th.batteryWarnPct) << ",\n"
        << "    \"battery_crit_pct\": " << jsonNumber(th.batteryCritPct) << ",\n"
        << "    \"power_warn_w\": " << jsonNumber(th.powerWarnW) << ",\n"
        << "    \"power_crit_w\": " << jsonNumber(th.powerCritW) << ",\n"
        << "    \"cpu_warn_pct\": " << jsonNumber(th.cpuWarnPct) << ",\n"
        << "    \"cpu_crit_pct\": " << jsonNumber(th.cpuCritPct) << ",\n"
        << "    \"gpu_warn_pct\": " << jsonNumber(th.gpuWarnPct) << ",\n"
        << "    \"gpu_crit_pct\": " << jsonNumber(th.gpuCritPct) << ",\n"
        << "    \"wifi_warn_dbm\": " << jsonNumber(th.wifiWarnDbm) << ",\n"
        << "    \"wifi_crit_dbm\": " << jsonNumber(th.wifiCritDbm) << ",\n"
        << "    \"controller_warn_pct\": " << jsonNumber(th.controllerWarnPct) << ",\n"
        << "    \"controller_crit_pct\": " << jsonNumber(th.controllerCritPct) << "\n"
        << "  },\n"
        << linkJson(config.link)  // T06
        << "  \"layout\": " << fph::layoutToJson(config.layout, 2) << "\n"
        << "}\n";

    // T04: temp file + fsync + rename (core/atomic_file), since PartOf=steamvr.service can stop us mid-save
    return fph::atomicWriteFile(path, out.str(), error);
}

void resetDisplaySettings(Config& config) {
    const Config defaults;
    config.visible = defaults.visible;
    config.attachment = defaults.attachment;
    config.posX = defaults.posX;
    config.posY = defaults.posY;
    config.posZ = defaults.posZ;
    config.yawDeg = defaults.yawDeg;
    config.pitchDeg = defaults.pitchDeg;
    config.rollDeg = defaults.rollDeg;
    config.leftWrist = defaults.leftWrist;
    config.rightWrist = defaults.rightWrist;
    config.wristFade = defaults.wristFade;
    config.wristFadeEndDeg = defaults.wristFadeEndDeg;
    config.clockFormat = defaults.clockFormat;
    config.widthM = defaults.widthM;
    config.alpha = defaults.alpha;
    config.detachedCurveDeg = defaults.detachedCurveDeg;  // R3
}

namespace {

constexpr double kDegToRad = M_PI / 180.0;

/**
 * 0.1° 単位に丸める（小数の誤差を設定ファイルに残さない）。
 * @param degrees 角度（度）
 * @return 丸めた値
 */
double roundTenthDegree(double degrees) {
    return std::round(degrees * 10.0) / 10.0 + 0.0;  // + 0.0 で −0 を 0 にする（ファイルに "-0" と書かない）
}

/**
 * faceHead() の yaw と pitch を計算する（丸める前の値）。
 * @param config 設定（位置だけ使う）
 * @param yaw yaw の書き込み先（度）
 * @param pitch pitch の書き込み先（度）
 */
void headFacingAngles(const Config& config, double& yaw, double& pitch) {
    // 原点と同じ位置のときは atan2(0, 0) = 0 になり、正面向きのまま
    yaw = std::atan2(-config.posX, -config.posZ) / kDegToRad;
    pitch = std::atan2(-config.posY, std::hypot(config.posX, config.posZ)) / kDegToRad;
}

}  // namespace

void panelRotation(double yawDeg, double pitchDeg, double rollDeg, double r[3][3]) {
    const double cy = std::cos(yawDeg * kDegToRad), sy = std::sin(yawDeg * kDegToRad);
    const double cp = std::cos(pitchDeg * kDegToRad), sp = std::sin(pitchDeg * kDegToRad);
    const double cr = std::cos(rollDeg * kDegToRad), sr = std::sin(rollDeg * kDegToRad);
    // Ry(yaw) = [cy 0 sy; 0 1 0; −sy 0 cy]、Rx(−pitch) = [1 0 0; 0 cp sp; 0 −sp cp]、Rz(roll) = [cr −sr 0; sr cr 0; 0 0 1]
    // を掛けた R = Ry · Rx(−pitch) · Rz。3 列目（パネルの表の向き）は (sy·cp, sp, cy·cp)
    r[0][0] = cy * cr - sy * sp * sr;
    r[0][1] = -cy * sr - sy * sp * cr;
    r[0][2] = sy * cp;
    r[1][0] = cp * sr;
    r[1][1] = cp * cr;
    r[1][2] = sp;
    r[2][0] = -sy * cr - cy * sp * sr;
    r[2][1] = sy * sr - cy * sp * cr;
    r[2][2] = cy * cp;
}

void panelRotation(const Config& config, double r[3][3]) {
    panelRotation(config.yawDeg, config.pitchDeg, config.rollDeg, r);
}

void faceHead(Config& config) {
    double yaw = 0.0;
    double pitch = 0.0;
    headFacingAngles(config, yaw, pitch);
    config.yawDeg = roundTenthDegree(yaw);
    config.pitchDeg = roundTenthDegree(pitch);
}

bool isFacingHead(const Config& config) {
    double yaw = 0.0;
    double pitch = 0.0;
    headFacingAngles(config, yaw, pitch);
    return std::fabs(config.yawDeg - yaw) < 0.25 && std::fabs(config.pitchDeg - pitch) < 0.25;
}

bool ConfigWatcher::save(const Config& config, std::string& error) {
    if (loadFailed_) {
        const std::string bad = path_ + ".bad";
        if (std::rename(path_.c_str(), bad.c_str()) == 0) {
            std::fprintf(stderr, "[config] %s couldn't be read; kept it as %s and saving a fresh one\n", path_.c_str(),
                         bad.c_str());
        } else if (errno != ENOENT) {
            error = "can't keep the unreadable config as " + bad + ": " + std::strerror(errno);
            return false;
        }
        loadFailed_ = false;
    }
    if (!saveConfig(path_, config, error)) return false;
    noteSaved();
    return true;
}

void ConfigWatcher::noteSaved() {
    statFile(existed_, mtime_, size_);
}

ConfigWatcher::ConfigWatcher(std::string path) : path_(std::move(path)) {}

void ConfigWatcher::statFile(bool& exists, struct timespec& mtime, off_t& size) const {
    struct stat st {};
    exists = ::stat(path_.c_str(), &st) == 0;
    mtime = exists ? st.st_mtim : timespec{};
    size = exists ? st.st_size : 0;
}

bool ConfigWatcher::loadAndLog(Config& config) const {
    std::vector<std::string> warnings;
    std::string error;
    Config loaded;
    loadFailed_ = false;
    if (!loadConfig(path_, loaded, warnings, error)) {
        loadFailed_ = true;
        std::fprintf(stderr, "[config] couldn't read %s: %s (keeping previous settings)\n", path_.c_str(), error.c_str());
        return false;
    }
    for (const auto& warning : warnings) std::fprintf(stderr, "[config] %s\n", warning.c_str());
    config = loaded;
    return true;
}

Config ConfigWatcher::loadInitial() {
    statFile(existed_, mtime_, size_);
    Config config;
    if (existed_) {
        if (loadAndLog(config)) std::fprintf(stderr, "[config] loaded %s\n", path_.c_str());
    } else {
        std::fprintf(stderr, "[config] %s not found, using defaults\n", path_.c_str());
    }
    return config;
}

bool ConfigWatcher::reloadIfChanged(Config& config) {
    bool exists = false;
    struct timespec mtime {};
    off_t size = 0;
    statFile(exists, mtime, size);
    const bool changed = exists != existed_ || size != size_ || mtime.tv_sec != mtime_.tv_sec ||
                         mtime.tv_nsec != mtime_.tv_nsec;
    if (!changed) return false;
    existed_ = exists;
    mtime_ = mtime;
    size_ = size;
    if (!exists) {
        std::fprintf(stderr, "[config] %s was removed, back to defaults\n", path_.c_str());
        config = Config();
        return true;
    }
    if (!loadAndLog(config)) return false;
    std::fprintf(stderr, "[config] reloaded changes\n");
    return true;
}
