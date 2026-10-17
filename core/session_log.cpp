#include "session_log.h"

#include "atomic_file.h"
#include "stream_metrics.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>

namespace fph {
namespace slog {

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

bool isNum(float v) {
    return v == v;
}

/** @return the metric's dotted name ("frame.app_fps"), or its id as text if this build doesn't know it */
std::string nameOf(uint16_t id) {
    const MetricDef* d = findMetric(id);
    return d != nullptr ? d->name : std::to_string(id);
}

const char* unitOf(uint16_t id) {
    const MetricDef* d = findMetric(id);
    return d != nullptr ? d->unit : "";
}

/** @return "12.3" (%.*f), or "null" for NaN */
std::string jsonNum(double v, int decimals = 3) {
    if (!(v == v) || std::isinf(v)) return "null";
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, v);
    // trim trailing zeros: 12.300 -> 12.3, 4.000 -> 4
    std::string s = buffer;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

std::string jsonStr(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

std::tm localTm(int64_t unixTime) {
    const std::time_t t = static_cast<std::time_t>(unixTime);
    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::string formatTime(int64_t unixTime, const char* format) {
    const std::tm tm = localTm(unixTime);
    char buffer[48];
    std::strftime(buffer, sizeof(buffer), format, &tm);
    return buffer;
}

/** @return dir + separator + name, using the separator dir already uses */
std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char sep = dir.find('\\') != std::string::npos ? '\\' : '/';
    return dir.back() == sep ? dir + name : dir + sep + name;
}

/** "%.5g", "" for NaN (CSV) */
void appendCsvValue(std::string& out, float v) {
    if (!isNum(v)) return;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", static_cast<double>(v));
    out += buffer;
}

int columnIndex(const std::vector<LogColumn>& columns, uint16_t id) {
    for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

/** @return a histogram percentile kept inside the values actually seen (a bin's width can reach past them) */
double clampTo(double v, const ColumnStats& st) {
    return v == v ? std::clamp(v, static_cast<double>(st.min), static_cast<double>(st.max)) : v;
}

std::string durationText(int64_t seconds) {
    char buffer[32];
    if (seconds >= 3600) {
        std::snprintf(buffer, sizeof(buffer), "%lld h %02lld min", static_cast<long long>(seconds / 3600),
                      static_cast<long long>(seconds % 3600 / 60));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld min %02lld s", static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60));
    }
    return buffer;
}

}  // namespace

// ---- sampling ----

std::vector<float> sampleColumns(const MetricStore& store, const std::vector<LogColumn>& columns, double now) {
    std::vector<float> out(columns.size(), kNaN);
    for (size_t i = 0; i < columns.size(); ++i) {
        const MetricValue* v = store.get(columns[i].id);
        if (v == nullptr || !v->ok || v->sampledAt < 0) continue;
        const MetricDef* d = findMetric(columns[i].id);
        const double interval = d != nullptr ? d->intervalMs / 1000.0 : 1.0;
        if (now - v->sampledAt > std::max(3.0, 2.5 * interval)) continue;  // the source stopped: a gap, not its last value
        switch (v->type) {
            case WireType::F32: out[i] = v->f32; break;
            case WireType::I32: out[i] = static_cast<float>(v->i32); break;
            case WireType::Enum: out[i] = static_cast<float>(v->enumValue); break;
            case WireType::ShortString: break;
        }
    }
    return out;
}

bool anyValue(const std::vector<float>& values) {
    for (float v : values) {
        if (isNum(v)) return true;
    }
    return false;
}

// ---- histogram ----

Histogram::Histogram(float min, float max, uint16_t bins) : min_(min), max_(max), counts_(bins, 0) {}

void Histogram::add(float v) {
    if (counts_.empty() || !isNum(v) || !(max_ > min_)) return;
    const double pos = (static_cast<double>(v) - min_) / (static_cast<double>(max_) - min_) * counts_.size();
    const long idx = static_cast<long>(std::floor(pos));
    const size_t i = static_cast<size_t>(std::clamp<long>(idx, 0, static_cast<long>(counts_.size()) - 1));
    ++counts_[i];
    ++total_;
}

void Histogram::merge(const Histogram& other) {
    if (other.counts_.size() != counts_.size()) return;
    for (size_t i = 0; i < counts_.size(); ++i) counts_[i] += other.counts_[i];
    total_ += other.total_;
}

double Histogram::percentile(double p) const {
    if (total_ == 0) return std::numeric_limits<double>::quiet_NaN();
    const double target = std::clamp(p, 0.0, 1.0) * static_cast<double>(total_);
    const double width = (static_cast<double>(max_) - min_) / counts_.size();
    double below = 0;
    for (size_t i = 0; i < counts_.size(); ++i) {
        const double c = counts_[i];
        if (c > 0 && below + c >= target) {
            const double frac = std::clamp((target - below) / c, 0.0, 1.0);
            return min_ + (static_cast<double>(i) + frac) * width;
        }
        below += c;
    }
    return max_;
}

// ---- stats ----

void ColumnStats::add(LogKind kind, float v) {
    ++samples;
    sum += v;
    if (samples == 1) {
        min = max = first = v;
    } else {
        min = std::min(min, v);
        max = std::max(max, v);
    }
    switch (kind) {
        case LogKind::Gauge: break;
        case LogKind::Count: counted += v; break;
        case LogKind::Cumulative:
            // a counter that went down was restarted (the reader or vrlink): it counted v since then
            if (samples > 1 && !rebase) counted += v >= last ? v - last : v;
            rebase = false;
            break;
        case LogKind::Flag:
            if (v != 0 && !high) counted += 1;
            high = v != 0;
            break;
    }
    last = v;
}

void ColumnStats::merge(LogKind kind, const ColumnStats& later, bool bridge) {
    if (later.samples == 0) {
        // nothing read during the detour, but a gap before it still means the next value is a new baseline
        if (kind == LogKind::Cumulative && (!bridge || later.rebase)) rebase = true;
        return;
    }
    if (samples == 0) {
        *this = later;
        return;
    }
    if (kind == LogKind::Cumulative && bridge && !rebase) {
        counted += later.first >= last ? later.first - last : later.first;
    }
    rebase = later.rebase;
    samples += later.samples;
    sum += later.sum;
    min = std::min(min, later.min);
    max = std::max(max, later.max);
    counted += later.counted;
    last = later.last;
    high = later.high;
}

// ---- timeline ----

Timeline::Timeline(std::vector<LogKind> kinds, size_t capBytes)
    : kinds_(std::move(kinds)), cap_(capBytes), acc_(kinds_.size(), 0.0), accN_(kinds_.size(), 0) {}

void Timeline::push(int64_t offset, int duration, const float* values) {
    const size_t capFloats = cap_ / sizeof(float);
    if (data_.size() + stride() > data_.capacity()) {
        // grow by doubling, but never past the cap (+ one row): RAM stays near cap_, not 2× it
        const size_t want = std::max<size_t>(data_.capacity() * 2, stride() * 256);
        data_.reserve(std::max(data_.size() + stride(), std::min(want, capFloats + stride())));
    }
    data_.push_back(static_cast<float>(offset));
    data_.push_back(static_cast<float>(duration));
    data_.insert(data_.end(), values, values + kinds_.size());
    if (data_.size() * sizeof(float) > cap_) halve();
}

void Timeline::halve() {
    const size_t n = rows();
    const size_t s = stride();
    size_t out = 0;
    for (size_t i = 0; i < n; i += 2, ++out) {
        float* dst = &data_[out * s];
        const float* a = &data_[i * s];
        if (i + 1 >= n) {
            if (dst != a) std::copy(a, a + s, dst);
            continue;
        }
        const float* b = &data_[(i + 1) * s];
        const float da = a[1], db = b[1];
        float merged[2] = {a[0], b[0] + b[1] - a[0]};
        std::vector<float> v(kinds_.size());
        for (size_t c = 0; c < kinds_.size(); ++c) {
            const float x = a[2 + c], y = b[2 + c];
            if (!isNum(x)) {
                v[c] = y;
            } else if (!isNum(y)) {
                v[c] = x;
            } else {
                switch (kinds_[c]) {
                    case LogKind::Gauge: v[c] = (x * da + y * db) / (da + db); break;
                    case LogKind::Count: v[c] = x + y; break;
                    case LogKind::Cumulative: v[c] = y; break;
                    case LogKind::Flag: v[c] = std::max(x, y); break;
                }
            }
        }
        dst[0] = merged[0];
        dst[1] = merged[1];
        std::copy(v.begin(), v.end(), dst + 2);
    }
    data_.resize(out * s);
    step_ *= 2;
}

void Timeline::add(int64_t offset, const std::vector<float>& values) {
    if (offset <= lastOffset_ || values.size() != kinds_.size()) return;
    lastOffset_ = offset;
    const int want = offset >= kFullRateSec ? std::max(step_, kDownsampledStepSec) : step_;
    if (want != step_) {
        flush();
        step_ = want;
    }
    if (step_ == 1 && accStart_ < 0) {
        push(offset, 1, values.data());
        return;
    }
    if (accStart_ >= 0 && offset - accStart_ >= step_) flush();  // this sample starts the next row
    if (accStart_ < 0) accStart_ = offset;
    accEnd_ = offset;
    for (size_t c = 0; c < kinds_.size(); ++c) {
        const float v = values[c];
        if (!isNum(v)) continue;
        switch (kinds_[c]) {
            case LogKind::Gauge:
            case LogKind::Count: acc_[c] += v; break;
            case LogKind::Cumulative: acc_[c] = v; break;
            case LogKind::Flag: acc_[c] = accN_[c] ? std::max(acc_[c], static_cast<double>(v)) : v; break;
        }
        ++accN_[c];
    }
    if (accEnd_ - accStart_ + 1 >= step_) flush();
}

void Timeline::flush() {
    if (accStart_ < 0) return;
    std::vector<float> row(kinds_.size(), kNaN);
    for (size_t c = 0; c < kinds_.size(); ++c) {
        if (accN_[c] == 0) continue;
        row[c] = static_cast<float>(kinds_[c] == LogKind::Gauge ? acc_[c] / accN_[c] : acc_[c]);
    }
    const int64_t start = accStart_;
    const int duration = static_cast<int>(accEnd_ - accStart_ + 1);
    std::fill(acc_.begin(), acc_.end(), 0.0);
    std::fill(accN_.begin(), accN_.end(), 0u);
    accStart_ = accEnd_ = -1;
    push(start, duration, row.data());
}

void Timeline::append(const Timeline& other, int64_t delta) {
    if (other.kinds_.size() != kinds_.size()) return;
    flush();
    Timeline src = other;
    src.flush();
    for (size_t i = 0; i < src.rows(); ++i) {
        const TimelineRow r = src.row(i);
        if (r.offset + delta <= lastOffset_) continue;
        push(r.offset + delta, r.duration, r.values);
        lastOffset_ = r.offset + delta + r.duration - 1;
    }
    step_ = std::max(step_, src.step_);
}

size_t Timeline::rows() const {
    return kinds_.empty() ? 0 : data_.size() / stride();
}

TimelineRow Timeline::row(size_t i) const {
    const float* r = &data_[i * stride()];
    TimelineRow out;
    out.offset = static_cast<int64_t>(r[0]);
    out.duration = static_cast<int>(r[1]);
    out.values = r + 2;
    return out;
}

// ---- session log ----

SessionLog::SessionLog(std::vector<LogColumn> columns, std::string source)
    : columns_(std::move(columns)), source_(std::move(source)) {
    for (const LogColumn& c : columns_) kinds_.push_back(c.kind);
}

std::unique_ptr<SessionData> SessionLog::start(const SessionInfo& info) const {
    auto s = std::make_unique<SessionData>();
    s->info = info;
    s->source = source_;
    s->stats.resize(columns_.size());
    for (const LogColumn& c : columns_) s->hist.emplace_back(c.histMin, c.histMax, c.bins);
    s->timeline = Timeline(kinds_);
    return s;
}

void SessionLog::end(std::unique_ptr<SessionData> s) {
    if (!s) return;
    s->timeline.flush();
    ended_.push_back(std::move(s));
}

void SessionLog::observe(const SessionInfo& in, int64_t nowUnix) {
    if (in.id == 0) {
        endAll();  // T11: no app for a whole detour (or no session at all): its end is when the app left
        return;
    }
    SessionInfo session = in;
    if (session.leftUnix == 0 && session.appKey.empty()) {
        // LINK_STATE has no leftUnix: the PC copy's app left when it first saw the key empty
        const SessionData* known = current_ && current_->info.id == in.id ? current_.get() : nullptr;
        session.leftUnix = known && known->info.leftUnix != 0 ? known->info.leftUnix : nowUnix;
    }
    if (!current_) {
        current_ = start(session);
    } else if (session.id == current_->info.id) {
        if (!session.appKey.empty()) current_->info.appKey = session.appKey;
        if (!session.appName.empty()) current_->info.appName = session.appName;
        if (current_->info.leftUnix != 0 && session.leftUnix == 0) {
            // back after no app: what a running total rose by meanwhile isn't the session's. Pending per column,
            // until that column's next value (a counter can come back later than the rest)
            for (ColumnStats& st : current_->stats) st.rebase = true;
        }
        current_->info.leftUnix = session.leftUnix;
    } else if (parked_ && parked_->info.id == session.id) {
        // Back after a short detour (SessionTracker resumed the old id): the detour's data joins the game's
        SessionData& game = *parked_;
        SessionData& detour = *current_;
        game.timeline.append(detour.timeline, detour.info.startUnix - game.info.startUnix);
        // T11: a no-app stretch on either side of the detour isn't counted: the game had left before it (no bridge
        // from the game's last value), or the detour's app had left before the game came back (a new baseline)
        const bool bridge = game.info.leftUnix == 0;
        for (size_t c = 0; c < columns_.size(); ++c) {
            game.stats[c].merge(kinds_[c], detour.stats[c], bridge);
            if (detour.info.leftUnix != 0) game.stats[c].rebase = true;
            game.hist[c].merge(detour.hist[c]);
        }
        game.dataSeconds += detour.dataSeconds;
        game.lastUnix = std::max(game.lastUnix, detour.lastUnix);
        if (game.note.empty()) game.note = detour.note;
        if (!session.appName.empty()) game.info.appName = session.appName;
        game.info.leftUnix = session.leftUnix;
        current_ = std::move(parked_);
    } else {
        end(std::move(parked_));
        parked_ = std::move(current_);
        current_ = start(session);
    }
    // the new session outlived a detour: the one before it can't come back any more
    if (parked_ && nowUnix - current_->info.startUnix >= kSessionDetourSec) end(std::move(parked_));
}

void SessionLog::add(int64_t nowUnix, const std::vector<float>& values) {
    if (!current_ || values.size() != columns_.size() || !anyValue(values)) return;
    if (current_->info.leftUnix != 0) return;  // T11: its app is gone: these seconds aren't the game's
    SessionData& s = *current_;
    if (s.dataSeconds > 0 && nowUnix <= s.lastUnix) return;  // once per second
    for (size_t c = 0; c < columns_.size(); ++c) {
        if (!isNum(values[c])) continue;
        s.stats[c].add(kinds_[c], values[c]);
        s.hist[c].add(values[c]);
    }
    s.timeline.add(std::max<int64_t>(0, nowUnix - s.info.startUnix), values);
    ++s.dataSeconds;
    s.lastUnix = nowUnix;
}

void SessionLog::endAll() {
    end(std::move(parked_));
    end(std::move(current_));
}

std::vector<std::unique_ptr<SessionData>> SessionLog::takeEnded() {
    std::vector<std::unique_ptr<SessionData>> out;
    out.swap(ended_);
    return out;
}

size_t SessionLog::bytes() const {
    return (current_ ? current_->timeline.bytes() : 0) + (parked_ ? parked_->timeline.bytes() : 0);
}

// ---- recent ring ----

RecentRing::RecentRing(size_t columns, int seconds)
    : columns_(columns), capacity_(static_cast<size_t>(seconds)), seconds_(seconds), values_(columns * capacity_, kNaN),
      times_(capacity_, 0) {}

void RecentRing::add(int64_t unixTime, const std::vector<float>& values) {
    if (values.size() != columns_) return;
    if (count_ > 0 && unixTime <= times_[(next_ + capacity_ - 1) % capacity_]) return;
    std::copy(values.begin(), values.end(), values_.begin() + static_cast<std::ptrdiff_t>(next_ * columns_));
    times_[next_] = unixTime;
    next_ = (next_ + 1) % capacity_;
    count_ = std::min(count_ + 1, capacity_);
}

size_t RecentRing::sizeWithin(int64_t nowUnix) const {
    size_t n = 0;
    for (size_t k = 0; k < count_; ++k) {
        if (times_[(next_ + capacity_ - 1 - k) % capacity_] > nowUnix - seconds_) ++n;
    }
    return n;
}

Timeline RecentRing::timeline(const std::vector<LogKind>& kinds, int64_t& startUnix, int64_t nowUnix) const {
    Timeline t(kinds, std::numeric_limits<size_t>::max() / 2);
    const size_t inWindow = sizeWithin(nowUnix);  // the newest ones: times only grow
    const size_t first = (next_ + capacity_ - inWindow) % capacity_;
    startUnix = inWindow ? times_[first] : nowUnix;
    for (size_t k = 0; k < inWindow; ++k) {
        const size_t i = (first + k) % capacity_;
        std::vector<float> row(values_.begin() + static_cast<std::ptrdiff_t>(i * columns_),
                               values_.begin() + static_cast<std::ptrdiff_t>((i + 1) * columns_));
        t.add(times_[i] - startUnix, row);
    }
    return t;
}

// ---- files ----

std::string fileTime(int64_t unixTime) {
    return formatTime(unixTime, "%Y-%m-%d_%H%M%S");
}

std::string clockTime(int64_t unixTime) {
    return formatTime(unixTime, "%H:%M:%S");
}

std::string fileSafeApp(const SessionInfo& info) {
    const std::string& name = info.appName.empty() ? info.appKey : info.appName;
    std::string out;
    for (const char c : name) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                          c == '-';
        // '.' stays out: the stem ends at the first '.', which pruning relies on
        if (safe) out += c;
        else if (out.empty() || out.back() != '-') out += '-';
        if (out.size() >= 40) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "app" : out;
}

int64_t endUnix(const SessionData& s) {
    return std::max(s.info.leftUnix != 0 ? s.info.leftUnix : s.lastUnix, s.info.startUnix);
}

std::string summaryJson(const SessionData& s, const std::vector<LogColumn>& columns) {
    std::ostringstream o;
    const int64_t end = endUnix(s);
    o << "{\n";
    o << "  \"format\": \"frame-perf-hud session 1\",\n";
    o << "  \"source\": " << jsonStr(s.source) << ",\n";
    o << "  \"app_key\": " << jsonStr(s.info.appKey) << ",\n";
    o << "  \"app_name\": " << jsonStr(s.info.appName) << ",\n";
    o << "  \"session_id\": " << s.info.id << ",\n";
    o << "  \"start\": " << jsonStr(formatTime(s.info.startUnix, "%Y-%m-%d %H:%M:%S")) << ",\n";
    o << "  \"start_unix\": " << s.info.startUnix << ",\n";
    o << "  \"end_unix\": " << end << ",\n";
    o << "  \"duration_s\": " << end - s.info.startUnix << ",\n";
    o << "  \"data_s\": " << s.dataSeconds << ",\n";
    o << "  \"timeline_step_s\": " << s.timeline.step() << ",\n";
    o << "  \"note\": " << jsonStr(s.note) << ",\n";
    o << "  \"card\": [";
    const std::vector<std::string> card = endCard(s, columns);
    for (size_t i = 0; i < card.size(); ++i) o << (i ? ", " : "") << jsonStr(card[i]);
    o << "],\n";
    o << "  \"metrics\": {";
    bool firstMetric = true;
    for (size_t c = 0; c < columns.size() && c < s.stats.size(); ++c) {
        const ColumnStats& st = s.stats[c];
        if (st.samples == 0) continue;
        o << (firstMetric ? "\n" : ",\n") << "    " << jsonStr(nameOf(columns[c].id)) << ": {";
        firstMetric = false;
        o << "\"unit\": " << jsonStr(unitOf(columns[c].id)) << ", \"samples\": " << st.samples;
        switch (columns[c].kind) {
            case LogKind::Gauge:
                o << ", \"avg\": " << jsonNum(st.avg()) << ", \"min\": " << jsonNum(st.min)
                  << ", \"max\": " << jsonNum(st.max)
                  << ", \"p5\": " << jsonNum(clampTo(s.hist[c].percentile(0.05), st))
                  << ", \"p95\": " << jsonNum(clampTo(s.hist[c].percentile(0.95), st));
                break;
            case LogKind::Count: o << ", \"total\": " << jsonNum(st.counted, 0); break;
            case LogKind::Cumulative: o << ", \"count\": " << jsonNum(st.counted, 0); break;
            case LogKind::Flag:
                o << ", \"events\": " << jsonNum(st.counted, 0) << ", \"share_pct\": " << jsonNum(st.avg() * 100.0, 1);
                break;
        }
        o << "}";
    }
    o << "\n  },\n";
    o << "  \"histograms\": {";
    bool firstHist = true;
    for (size_t c = 0; c < columns.size() && c < s.hist.size(); ++c) {
        const Histogram& h = s.hist[c];
        if (!columns[c].summaryHist || h.total() == 0) continue;
        o << (firstHist ? "\n" : ",\n") << "    " << jsonStr(nameOf(columns[c].id)) << ": {\"min\": " << jsonNum(h.min())
          << ", \"max\": " << jsonNum(h.max()) << ", \"bins\": [";
        firstHist = false;
        // trailing empty bins are left out (the bin width is (max − min) / the full count, written below)
        size_t used = h.counts().size();
        while (used > 0 && h.counts()[used - 1] == 0) --used;
        for (size_t i = 0; i < used; ++i) o << (i ? "," : "") << h.counts()[i];
        o << "], \"bin_count\": " << h.counts().size() << "}";
    }
    o << "\n  }\n}\n";
    return o.str();
}

std::string timelineCsv(const Timeline& t, const std::vector<LogColumn>& columns, int64_t startUnix) {
    const size_t n = std::min(columns.size(), t.columns());
    std::vector<bool> used(n, false);
    for (size_t i = 0; i < t.rows(); ++i) {
        const TimelineRow r = t.row(i);
        for (size_t c = 0; c < n; ++c) used[c] = used[c] || isNum(r.values[c]);
    }
    std::string out = "unix,t_s,dur_s";
    for (size_t c = 0; c < n; ++c) {
        if (used[c]) out += "," + nameOf(columns[c].id);
    }
    out += "\n";
    for (size_t i = 0; i < t.rows(); ++i) {
        const TimelineRow r = t.row(i);
        out += std::to_string(startUnix + r.offset) + "," + std::to_string(r.offset) + "," + std::to_string(r.duration);
        for (size_t c = 0; c < n; ++c) {
            if (!used[c]) continue;
            out += ",";
            appendCsvValue(out, r.values[c]);
        }
        out += "\n";
    }
    return out;
}

std::string gzip(const std::string& text) {
    z_stream zs {};
    // windowBits 15 + 16: a gzip header, so `gzip -d` and Python's gzip read it
    if (deflateInit2(&zs, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return std::string();
    std::string out(deflateBound(&zs, static_cast<uLong>(text.size())), '\0');
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data()));
    zs.avail_in = static_cast<uInt>(text.size());
    zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&zs, Z_FINISH);
    const size_t written = zs.total_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) return std::string();
    out.resize(written);
    return out;
}

std::vector<std::string> endCard(const SessionData& s, const std::vector<LogColumn>& columns) {
    std::vector<std::string> lines;
    const auto stat = [&](uint16_t id) -> const ColumnStats* {
        const int i = columnIndex(columns, id);
        return i >= 0 && static_cast<size_t>(i) < s.stats.size() && s.stats[static_cast<size_t>(i)].samples > 0
                   ? &s.stats[static_cast<size_t>(i)]
                   : nullptr;
    };
    char line[160];
    const std::string app = s.info.appName.empty() ? s.info.appKey : s.info.appName;
    std::snprintf(line, sizeof(line), "%s · %s", app.c_str(),
                  durationText(endUnix(s) - s.info.startUnix).c_str());
    lines.push_back(line);
    if (const ColumnStats* fps = stat(metric::kFrameAppFps)) {
        const ColumnStats* dropped = stat(metric::kFrameDropped);
        std::snprintf(line, sizeof(line), "fps avg %.1f · %.0f dropped", fps->avg(), dropped ? dropped->counted : 0.0);
        lines.push_back(line);
    }
    if (const ColumnStats* w = stat(metric::kPowerVphW)) {
        std::snprintf(line, sizeof(line), "Power avg %.1f W · peak %.1f W", w->avg(), static_cast<double>(w->max));
        lines.push_back(line);
    }
    if (const ColumnStats* b = stat(metric::kBatteryPct)) {
        std::snprintf(line, sizeof(line), "Battery %.0f%% to %.0f%% (%+.0f points)", static_cast<double>(b->first),
                      static_cast<double>(b->last), static_cast<double>(b->last - b->first));
        lines.push_back(line);
    }
    std::string temps;
    const std::pair<uint16_t, const char*> peaks[] = {
        {metric::kTempCpuC, "CPU"}, {metric::kTempGpuC, "GPU"}, {metric::kTempBatteryC, "battery"}};
    for (const auto& p : peaks) {
        if (const ColumnStats* t = stat(p.first)) {
            std::snprintf(line, sizeof(line), "%s%s %.0f °C", temps.empty() ? "" : " · ", p.second,
                          static_cast<double>(t->max));
            temps += line;
        }
    }
    if (!temps.empty()) lines.push_back("Peak " + temps);
    if (const ColumnStats* total = stat(metric::kPcStreamTotalAvg)) {
        std::string stream;
        std::snprintf(line, sizeof(line), "Stream latency avg %.1f ms", total->avg());
        stream = line;
        if (const ColumnStats* slack = stat(metric::kPcStreamSlackAvg)) {
            std::snprintf(line, sizeof(line), " · min slack %.1f ms", static_cast<double>(slack->min));
            stream += line;
        }
        if (const ColumnStats* resets = stat(metric::kPcStreamStreamResets)) {
            std::snprintf(line, sizeof(line), " · %.0f link reset%s", resets->counted, resets->counted == 1 ? "" : "s");
            stream += line;
        }
        lines.push_back(stream);
    }
    return lines;
}

bool writeSession(const std::string& dir, const std::string& stem, const SessionData& s,
                  const std::vector<LogColumn>& columns, int keep, std::string& error) {
    if (!makeDirectories(dir)) {
        error = "can't create " + dir;
        return false;
    }
    if (!atomicWriteFile(joinPath(dir, stem + ".json"), summaryJson(s, columns), error)) return false;
    const std::string gz = gzip(timelineCsv(s.timeline, columns, s.info.startUnix));
    if (gz.empty()) {
        error = "gzip failed";
        return false;
    }
    if (!atomicWriteFile(joinPath(dir, stem + ".csv.gz"), gz, error)) return false;
    pruneByCount(dir, keep);
    return true;
}

bool ownedLogFile(const std::string& name, std::string& stem) {
    // "YYYY-MM-DD_HHMMSS" (fileTime), then "_<app>[_save|_record|_crash]" from [A-Za-z0-9_-], then our suffix
    static const char kTime[] = "dddd-dd-dd_dddddd_";
    const size_t timeLen = sizeof(kTime) - 1;
    if (name.size() <= timeLen) return false;
    for (size_t i = 0; i < timeLen; ++i) {
        const bool ok = kTime[i] == 'd' ? (name[i] >= '0' && name[i] <= '9') : name[i] == kTime[i];
        if (!ok) return false;
    }
    const size_t dot = name.find('.');
    if (dot == std::string::npos || dot == timeLen) return false;
    const std::string suffix = name.substr(dot);
    if (suffix != ".json" && suffix != ".csv.gz" && suffix != ".csv") return false;
    for (size_t i = timeLen; i < dot; ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-';
        if (!ok) return false;
    }
    stem = name.substr(0, dot);
    return true;
}

void pruneByCount(const std::string& dir, int keep) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::u8path(dir);
    std::vector<std::pair<std::string, fs::path>> files;  // (stem, path)
    std::set<std::string> stems;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().u8string();
        std::string stem;
        if (!ownedLogFile(name, stem)) continue;  // anything else in the folder is never counted or deleted
        files.emplace_back(stem, it->path());
        stems.insert(stem);
    }
    if (static_cast<int>(stems.size()) <= keep) return;
    std::set<std::string> drop;
    for (const std::string& stem : stems) {
        if (static_cast<int>(stems.size() - drop.size()) <= keep) break;
        drop.insert(stem);  // std::set iterates in name order = time order, oldest first
    }
    for (const auto& f : files) {
        if (drop.count(f.first)) fs::remove(f.second, ec);
    }
}

// ---- record ----

RecordWriter::~RecordWriter() {
    close();
}

bool RecordWriter::open(const std::string& path, const std::vector<LogColumn>& columns, std::string& error) {
    close();
    const size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) makeDirectories(path.substr(0, slash));
    file_ = std::fopen(path.c_str(), "wb");
    if (file_ == nullptr) {
        error = "can't create " + path;
        return false;
    }
    path_ = path;
    rows_ = 0;
    pending_ = "unix";
    for (const LogColumn& c : columns) pending_ += "," + nameOf(c.id);
    pending_ += "\n";
    lastFlush_ = -1e18;
    return true;
}

void RecordWriter::add(int64_t unixTime, const std::vector<float>& values, double now, double flushSec) {
    if (file_ == nullptr) return;
    pending_ += std::to_string(unixTime);
    for (float v : values) {
        pending_ += ",";
        appendCsvValue(pending_, v);
    }
    pending_ += "\n";
    ++rows_;
    if (now - lastFlush_ >= flushSec) {
        std::fwrite(pending_.data(), 1, pending_.size(), file_);
        std::fflush(file_);
        pending_.clear();
        lastFlush_ = now;
    }
}

void RecordWriter::close() {
    if (file_ == nullptr) return;
    if (!pending_.empty()) std::fwrite(pending_.data(), 1, pending_.size(), file_);
    pending_.clear();
    std::fclose(file_);
    file_ = nullptr;
}

}  // namespace slog
}  // namespace fph
