#include "reader.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
namespace fph {
namespace pc {
namespace streaming {
const char kCsvHeader[] =
    "Present ID,WGP Release,App Submit,Encode Start,Encode End,Send Start,Send End,Receive,Decode Start,Decode "
    "End,Submit Illumination,Predicted Display,Decode Query,Run Start At WGP Degate,Ping At Send Start,Filter Ping At "
    "Send Start,Upstream Ping At Send Start,WGP Release (realtime),Receive (remote realtime),Receive (remote),Decode "
    "Start (remote),Decode End (remote),Submit Illumination (remote),Decode Query (remote),TX ID\n";
const char kCsvRow[] = "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d,%llu,%llu,%u,%u,%u,%u,%u,%d\n";
namespace {
template <class T> T at(const uint8_t* data, size_t offset) {
    T v;
    std::memcpy(&v, data + offset, sizeof(v));
    return v;
}
bool pointer(uint64_t p) {
    return p >= 0x10000 && p < 0x0000800000000000ull;
}
bool wantedKey(const std::string& key) {
    // Cache only the widget's values and the packet-rate cross-check. Validate every cached key each tick.
    static const std::set<std::string> keys = {"sPingMS",    "dbgEncTimeMS",     "txTimeMS",  "dEstLinkBW", "rexmitReq",
                                               "rexmitSent", "nFramesSkippedPS", "dFrmsLate", "encResets",  "sRXpps"};
    return keys.count(key) != 0;
}
struct Section {
    uint32_t rva, size, raw, rawSize;
};
struct Pe {
    const std::vector<uint8_t>& b;
    std::vector<Section> sections;
    uint32_t timestamp = 0, imageSize = 0, text = 0, textSize = 0, textRva = 0;
    uint64_t imageBase = 0;
    explicit Pe(const std::vector<uint8_t>& bytes) : b(bytes) {
        if (b.size() < 64 || at<uint16_t>(b.data(), 0) != 0x5a4d)
            return;
        const auto p = at<uint32_t>(b.data(), 0x3c);
        if (p > b.size() - 24 || at<uint32_t>(b.data(), p) != 0x4550 || at<uint16_t>(b.data(), p + 4) != 0x8664)
            return;
        const auto n = at<uint16_t>(b.data(), p + 6), opt = at<uint16_t>(b.data(), p + 20);
        if (n > 96 || opt < 60 || p + 24ull + opt + n * 40ull > b.size() || at<uint16_t>(b.data(), p + 24) != 0x20b)
            return;
        timestamp = at<uint32_t>(b.data(), p + 8);
        imageSize = at<uint32_t>(b.data(), p + 24 + 56);
        imageBase = at<uint64_t>(b.data(), p + 24 + 24);
        for (unsigned i = 0; i < n; ++i) {
            const size_t s = p + 24 + opt + i * 40;
            Section v{at<uint32_t>(b.data(), s + 12), at<uint32_t>(b.data(), s + 8), at<uint32_t>(b.data(), s + 20),
                      at<uint32_t>(b.data(), s + 16)};
            if (v.raw > b.size() || v.rawSize > b.size() - v.raw) {
                sections.clear();
                return;
            }
            sections.push_back(v);
            if (std::memcmp(b.data() + s, ".text\0", 6) == 0) {
                text = v.raw;
                textSize = v.rawSize;
                textRva = v.rva;
            }
        }
    }
    uint32_t rva(size_t offset) const {
        for (auto s : sections)
            if (offset >= s.raw && offset - s.raw < s.rawSize)
                return static_cast<uint32_t>(offset - s.raw + s.rva);
        return 0;
    }
    size_t raw(uint32_t va) const {
        for (auto s : sections)
            if (va >= s.rva && va - s.rva < s.rawSize)
                return va - s.rva + s.raw;
        return b.size();
    }
    std::vector<uint32_t> strings(const std::string& str) const {
        std::vector<uint32_t> out;
        const auto needle = str + std::string(1, '\0');
        for (size_t i = 0; i + needle.size() <= b.size(); ++i)
            if (std::memcmp(b.data() + i, needle.data(), needle.size()) == 0 && rva(i))
                out.push_back(rva(i));
        return out;
    }
    std::vector<size_t> refs(uint32_t va) const {
        std::vector<size_t> out;
        for (size_t i = text; i + 7 <= text + textSize; ++i)
            if ((b[i] == 0x48 || b[i] == 0x4c) && b[i + 1] == 0x8d && (b[i + 2] & 0xc7) == 5) {
                if (static_cast<int64_t>(rva(i)) + 7 + at<int32_t>(b.data(), i + 3) == va)
                    out.push_back(i);
            }
        return out;
    }
};
bool stdString(Memory& m, uint64_t address, std::string& out) {
    uint8_t b[32];
    if (!pointer(address) || !m.read(address, b, sizeof(b)))
        return false;
    const auto n = at<uint64_t>(b, 16), cap = at<uint64_t>(b, 24);
    if (n == 0 || n > 128 || cap < n || cap > 4096)
        return false;
    out.resize(static_cast<size_t>(n));
    if (cap < 16) {
        if (n >= 16)
            return false;
        std::memcpy(out.data(), b, n);
    } else {
        auto p = at<uint64_t>(b, 0);
        if (!pointer(p) || !m.read(p, out.data(), static_cast<size_t>(n)))
            return false;
    }
    return out.find('\0') == std::string::npos;
}
double average(const std::vector<double>& x) {
    return std::accumulate(x.begin(), x.end(), 0.0) / x.size();
}
double p95(std::vector<double> x) {
    std::sort(x.begin(), x.end());
    return x[static_cast<size_t>(std::ceil(x.size() * 0.95)) - 1];
}
void naRange(MetricStore& s, uint16_t first, uint16_t last, Reason r, double now) {
    for (const auto& d : registry())
        if (d.id >= first && d.id <= last && isPcStreamMetric(d.id))
            s.setNa(d.id, r, now);
}
} // namespace
Anchors findAnchors(const std::vector<uint8_t>& dll, const std::string& expectedHeader) {
    Anchors a;
    Pe p(dll);
    a.timestamp = p.timestamp;
    a.imageSize = p.imageSize;
    a.l1Note = "anchor mismatch";
    a.l2Note = "link anchor mismatch";
    a.l3Note = "CSV header/stride mismatch";
    if (!p.textSize || !a.imageSize)
        return a;
    const auto* b = dll.data();
    std::set<uint32_t> globals, maps, aftms, linkPings, uberPings, strides, files;
    for (size_t i = p.text; i + 14 <= p.text + p.textSize; ++i) {
        if (std::memcmp(b + i, "\x4c\x89\x3d", 3) == 0 && std::memcmp(b + i + 7, "\x48\x89\x1d", 3) == 0) {
            const int64_t g = static_cast<int64_t>(p.rva(i)) + 7 + at<int32_t>(b, i + 3),
                          g2 = static_cast<int64_t>(p.rva(i)) + 14 + at<int32_t>(b, i + 10);
            if (g2 - g == 0x40 && g > 0 && g + 8 < a.imageSize)
                globals.insert(static_cast<uint32_t>(g));
        }
    }
    for (auto str : p.strings("cPingAMS"))
        for (auto ref : p.refs(str)) {
            const size_t end = std::min(ref + 400, static_cast<size_t>(p.text + p.textSize));
            for (size_t j = ref; j + 7 <= end; ++j) {
                if (std::memcmp(b + j, "\x48\x8d\x8e", 3) == 0) {
                    auto off = at<uint32_t>(b, j + 3);
                    if (off < 0x100000)
                        aftms.insert(off);
                    break;
                }
            }
            for (size_t j = ref + 0x21; j + 5 <= end; ++j)
                if (b[j] == 0xe8) {
                    const int64_t target = static_cast<int64_t>(p.rva(j)) + 5 + at<int32_t>(b, j + 1);
                    if (target < 0 || target >= a.imageSize)
                        continue;
                    const size_t start = p.raw(static_cast<uint32_t>(target));
                    if (start + 0x60 > dll.size())
                        continue;
                    for (size_t k = start; k + 7 < start + 0x60; ++k)
                        if (std::memcmp(b + k, "\x48\x8b\xbf", 3) == 0) {
                            auto off = at<uint32_t>(b, k + 3);
                            if (off >= 8 && off < 0x10000)
                                maps.insert(off);
                        }
                }
        }
    for (auto str : p.strings("%.1f ms"))
        for (auto ref : p.refs(str)) {
            if (ref >= 6 && std::memcmp(b + ref - 6, "\x8b\x87", 2) == 0)
                uberPings.insert(at<uint32_t>(b, ref - 4));
            if (ref >= 3 && b[ref - 3] == 0x8b && b[ref - 2] == 0x46)
                linkPings.insert(b[ref - 1]);
        }
    for (auto str : p.strings(kCsvRow))
        for (auto ref : p.refs(str)) {
            const size_t begin = ref >= p.text + 0x1a0 ? ref - 0x1a0 : p.text;
            for (size_t j = begin; j + 7 <= ref; ++j) {
                if (std::memcmp(b + j, "\x48\x6b\xd0", 3) == 0)
                    strides.insert(b[j + 3]);
                if (std::memcmp(b + j, "\x4c\x8b\x91", 3) == 0)
                    files.insert(at<uint32_t>(b, j + 3));
            }
        }
    if (globals.size() == 1)
        a.serverGlobal = *globals.begin();
    if (maps.size() == 1)
        a.mapOffset = *maps.begin();
    if (aftms.size() == 1)
        a.aftmOffset = *aftms.begin();
    if (linkPings.size() == 1)
        a.linkPingOffset = *linkPings.begin();
    if (uberPings.size() == 1)
        a.uberPingOffset = *uberPings.begin();
    if (strides.size() == 1)
        a.stride = *strides.begin();
    if (files.size() == 1)
        a.fileOffset = *files.begin();
    a.l1 = a.serverGlobal && a.mapOffset && p.strings(".?AVSVLServer@@").size() == 1;
    a.l2 = a.linkPingOffset && a.uberPingOffset && p.strings(".?AVSVLDataLinkUber@@").size() == 1 &&
           p.strings(".?AVSVLDataLinkTransferUDP@@").size() == 1;
    a.l3 = a.aftmOffset && a.stride == kFrameStride && a.fileOffset == 0x88030 && p.strings(expectedHeader).size() == 1;
    // Locate the primary UDP vtable via x64 RTTI. Its remote-address getter (slot 0x50)
    // takes a sockaddr from this+offset, checks AF_INET/AF_INET6, then formats it.
    std::set<std::pair<uint32_t, uint32_t>> endpoints;
    for (const auto name : p.strings(".?AVSVLDataLinkTransferUDP@@")) {
        if (name < 16)
            continue;
        const uint32_t descriptor = name - 16;
        for (size_t i = 12; i + 12 <= dll.size(); ++i) {
            if (at<uint32_t>(b, i) != descriptor)
                continue;
            const size_t col = i - 12;
            const uint32_t colRva = p.rva(col);
            if (!colRva || at<uint32_t>(b, col) != 1 || at<uint32_t>(b, col + 4) != 0 ||
                at<uint32_t>(b, col + 20) != colRva)
                continue;
            const uint64_t locator = p.imageBase + colRva;
            for (size_t j = 0; j + 0x60 <= dll.size(); ++j) {
                if (at<uint64_t>(b, j) != locator)
                    continue;
                const uint64_t address = at<uint64_t>(b, j + 8 + 0x50);
                if (address < p.imageBase || address - p.imageBase >= p.imageSize)
                    continue;
                const size_t raw = p.raw(static_cast<uint32_t>(address - p.imageBase));
                if (raw < p.text || raw + 64 > p.text + p.textSize)
                    continue;
                for (size_t k = raw; k + 14 <= raw + 64; ++k) {
                    if (std::memcmp(b + k, "\x48\x8d\x91", 3) != 0 ||
                        std::memcmp(b + k + 7, "\x0f\xb7\x02\x66\x83\xf8\x02", 7) != 0)
                        continue;
                    const uint32_t offset = at<uint32_t>(b, k + 3);
                    if (offset >= 0x30 && offset <= 0x1000)
                        endpoints.insert({p.rva(k), offset});
                }
            }
        }
    }
    if (endpoints.size() == 1) {
        a.remoteGetterRva = endpoints.begin()->first;
        a.remoteAddressOffset = endpoints.begin()->second;
    }
    if (a.l1)
        a.l1Note.clear();
    if (a.l2)
        a.l2Note.clear();
    if (a.l3)
        a.l3Note.clear();
    return a;
}
bool sameLoadedBuild(Memory& m, uint64_t module, const Anchors& a) {
    uint8_t dos[64], nt[88];
    if (!m.read(module, dos, sizeof(dos)) || at<uint16_t>(dos, 0) != 0x5a4d)
        return false;
    const auto off = at<uint32_t>(dos, 0x3c);
    if (off < 64 || off > 4096 || !m.read(module + off, nt, sizeof(nt)))
        return false;
    return at<uint32_t>(nt, 0) == 0x4550 && at<uint16_t>(nt, 4) == 0x8664 && at<uint16_t>(nt, 24) == 0x20b &&
           at<uint32_t>(nt, 8) == a.timestamp && at<uint32_t>(nt, 80) == a.imageSize;
}
bool rttiIs(Memory& m, uint64_t obj, uint64_t module, uint32_t size, const char* expected) {
    uint64_t vt = 0, col = 0;
    uint8_t locator[24];
    if (!pointer(obj) || !m.get(obj, vt) || vt < module + 8 || vt >= module + size || !m.get(vt - 8, col) ||
        col < module || col + 24 > module + size || !m.read(col, locator, 24))
        return false;
    const auto td = at<uint32_t>(locator, 12);
    if (at<uint32_t>(locator, 0) != 1 || td + 16ull + std::strlen(expected) + 1 > size)
        return false;
    char name[96]{};
    const auto n = std::strlen(expected) + 1;
    return n <= sizeof(name) && m.read(module + td + 16, name, n) && std::memcmp(name, expected, n) == 0;
}
std::string readLinkEndpoint(Memory& memory, uint64_t linkObject, uint64_t module, const Anchors& anchors) {
    if (!anchors.remoteGetterRva || anchors.remoteGetterRva + 14ull > anchors.imageSize ||
        anchors.remoteAddressOffset < 0x30 || anchors.remoteAddressOffset > 0x1000 ||
        !rttiIs(memory, linkObject, module, anchors.imageSize, ".?AVSVLDataLinkTransferUDP@@"))
        return {};
    uint8_t code[14], address[16];
    if (!memory.read(module + anchors.remoteGetterRva, code, sizeof(code)) ||
        std::memcmp(code, "\x48\x8d\x91", 3) != 0 || std::memcmp(code + 7, "\x0f\xb7\x02\x66\x83\xf8\x02", 7) != 0 ||
        at<uint32_t>(code, 3) != anchors.remoteAddressOffset ||
        !memory.read(linkObject + anchors.remoteAddressOffset, address, sizeof(address)))
        return {};
    // AF_INET6 or an unrecognized address layout retains the generic Link N label.
    if (at<uint16_t>(address, 0) != 2 || (address[2] == 0 && address[3] == 0) || address[4] == 0 || address[4] == 127 ||
        address[4] >= 224)
        return {};
    const uint32_t ip = static_cast<uint32_t>(address[4]) << 24 | static_cast<uint32_t>(address[5]) << 16 |
                        static_cast<uint32_t>(address[6]) << 8 | address[7];
    return link::ipv4ToText(ip);
}
bool parseFrame(const uint8_t* b, Frame& f) {
    uint32_t t[13];
    std::memcpy(t, b, sizeof(t));
    if (!t[10] || !t[9] || !t[4] || !t[7])
        return false;
    f.id = t[0];
    f.illum = t[10];
    const unsigned pairs[7][2] = {{1, 2}, {3, 4}, {5, 6}, {6, 7}, {7, 9}, {9, 10}, {1, 10}};
    for (unsigned j = 0; j < 7; ++j) {
        const uint32_t us = t[pairs[j][1]] - t[pairs[j][0]];
        if (us > 500000)
            return false;
        f.ms[j] = us / 1000.0;
    }
    // Slack splits at Decode Query: hold waits for the compositor's run start, display is its fixed lead to
    // illumination. Hold is signed, since a frame can miss its slot.
    const int32_t hold = static_cast<int32_t>(t[12] - t[9]);
    const uint32_t display = t[10] - t[12];
    f.split = t[12] && hold > -500000 && hold < 500000 && display <= 500000;
    f.ms[7] = f.split ? hold / 1000.0 : 0;
    f.ms[8] = f.split ? display / 1000.0 : 0;
    const int32_t ping = at<int32_t>(b, 0x38);
    if (ping < 0 || ping > 500000)
        return false;
    f.ping = ping / 1000.0;
    return true;
}
void FrameWindow::clear() {
    samples_.clear();
    seen_.clear();
    ids_.clear();
    published_ = 0;
    newestIllum_ = 0;
    haveNewest_ = false;
}
void FrameWindow::add(const std::vector<uint8_t>& tail, double now) {
    if (tail.size() != kTailFrames * kFrameStride)
        return;
    // Completion order can differ from record order. An older record finishing late must never move
    // the cutoff backwards, including when the u32 illumination clock wraps.
    for (size_t j = 0; j < kTailFrames; ++j) {
        Frame f;
        if (!parseFrame(tail.data() + j * kFrameStride, f))
            continue;
        const uint32_t advance = f.illum - newestIllum_;
        if (!haveNewest_ || (advance != 0 && advance < 0x80000000u)) {
            newestIllum_ = f.illum;
            haveNewest_ = true;
        }
    }
    if (!haveNewest_)
        return;
    for (size_t j = 0; j < kTailFrames; ++j) {
        Frame f;
        if (!parseFrame(tail.data() + j * kFrameStride, f) ||
            static_cast<uint32_t>(newestIllum_ - f.illum) >= 1000000 || !seen_.insert(f.id).second)
            continue;
        ids_.push_back(f.id);
        samples_.push_back({f, now});
        ++published_;
    }
    while (ids_.size() > 8192) {
        seen_.erase(ids_.front());
        ids_.pop_front();
    }
    while (!samples_.empty() && now - samples_.front().received >= 1.0)
        samples_.pop_front();
}
std::vector<Frame> FrameWindow::recent(double now) const {
    std::vector<Frame> out;
    if (samples_.empty())
        return out;
    for (auto& x : samples_)
        if (now - x.received < 1.0 && static_cast<uint32_t>(newestIllum_ - x.frame.illum) < 1000000)
            out.push_back(x.frame);
    return out;
}
Reader::Reader(Memory& memory, uint64_t module, Anchors anchors)
    : memory_(memory), module_(module), anchors_(std::move(anchors)) {}
bool Reader::walkMap(uint64_t head) {
    nodes_.clear();
    uint64_t root = 0;
    if (!pointer(head) || !memory_.get(head + 8, root))
        return false;
    std::vector<uint64_t> stack{root};
    std::set<uint64_t> seen;
    while (!stack.empty()) {
        const auto n = stack.back();
        stack.pop_back();
        if (n == head)
            continue;
        if (!pointer(n) || !seen.insert(n).second || seen.size() > 512) {
            nodes_.clear();
            return false;
        }
        uint8_t b[0x48];
        if (!memory_.read(n, b, sizeof(b))) {
            nodes_.clear();
            return false;
        }
        if (b[0x19])
            continue;
        std::string key;
        if (!stdString(memory_, n + 0x20, key) || nodes_.count(key)) {
            nodes_.clear();
            return false;
        }
        if (wantedKey(key))
            nodes_[key] = n;
        stack.push_back(at<uint64_t>(b, 0));
        stack.push_back(at<uint64_t>(b, 0x10));
    }
    walkedSize_ = mapSize_;
    return !nodes_.empty();
}
bool Reader::named(std::map<std::string, double>& values) {
    // Keys keep arriving for seconds after vrserver starts (dbgEncTimeMS comes with the encoder), and cached
    // nodes stay valid meanwhile. Walk again whenever the map's element count changes.
    if ((nodes_.empty() || walkedSize_ != mapSize_) && !walkMap(head_))
        return false;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        values.clear();
        bool good = true;
        for (const auto& kv : nodes_) {
            uint8_t b[0x28];
            std::string key;
            if (!memory_.read(kv.second + 0x20, b, sizeof(b))) {
                good = false;
                break;
            }
            const auto len = at<uint64_t>(b, 16), cap = at<uint64_t>(b, 24);
            if (len > 0 && len < 16 && cap < 16 && cap >= len)
                key.assign(reinterpret_cast<const char*>(b), static_cast<size_t>(len));
            else if (!stdString(memory_, kv.second + 0x20, key)) {
                good = false;
                break;
            }
            if (key != kv.first) {
                good = false;
                break;
            }
            auto v = at<double>(b, 0x20);
            if (!std::isfinite(v) || std::abs(v) > 1e15) {
                good = false;
                break;
            }
            values[key] = v;
        }
        if (good)
            return true;
        if (!walkMap(head_))
            return false;
    }
    return false;
}
void Reader::tick(double now, double displayHz, MetricStore& out) {
    using namespace metric;
    out.setText(kPcStreamBuild, std::to_string(anchors_.timestamp), now);
    naRange(out, 1021, 1067, Reason::SteamVrChanged, now);
    naRange(out, 1090, 1093, Reason::NotConnected, now);
    out.setText(kPcStreamL1Note, anchors_.l1Note, now);
    out.setText(kPcStreamL2Note, anchors_.l2Note, now);
    out.setText(kPcStreamL3Note, anchors_.l3Note, now);
    if (!sameLoadedBuild(memory_, module_, anchors_)) {
        out.setText(kPcStreamL1Note, "loaded/disk build mismatch", now);
        out.setText(kPcStreamL2Note, "loaded/disk build mismatch", now);
        out.setText(kPcStreamL3Note, "loaded/disk build mismatch", now);
        return;
    }
    uint64_t srv = 0, head = 0, root = 0, uber = 0, bw = 0, size = 0;
    std::array<uint64_t, 16> links{};
    uint16_t index = 0;
    // MSVC std::map is {_Myhead, _Mysize}: the element count sits right after the head pointer.
    if (!anchors_.l1 || !memory_.get(module_ + anchors_.serverGlobal, srv) ||
        !rttiIs(memory_, srv, module_, anchors_.imageSize, ".?AVSVLServer@@") ||
        !memory_.get(srv + anchors_.mapOffset, head) || !pointer(head) || !memory_.get(head + 8, root) ||
        !memory_.get(srv + anchors_.mapOffset + 8, size)) {
        nodes_.clear();
        frames_.clear();
        out.setText(kPcStreamL1Note, "server RTTI/map mismatch", now);
        return;
    }
    const bool haveUber = memory_.get(srv + 0x188, uber) && pointer(uber);
    const bool haveLinks = haveUber && memory_.read(uber + 0x388, links.data(), sizeof(links));
    const bool haveBw = memory_.get(srv + 0x1f0, bw) && pointer(bw);
    const bool haveIndex = anchors_.l3 && memory_.get(srv + anchors_.aftmOffset + 0x88000, index) && index < 4096;
    const unsigned advance = haveIndex_ ? (index - index_) & 0xfff : 0;
    const bool changed = server_ && (server_ != srv || head_ != head || root_ != root || uber_ != uber || bw_ != bw ||
                                     links_ != links || (haveIndex_ && haveIndex && advance > 2048));
    if (changed) {
        ++resets_;
        nodes_.clear();
        frames_.clear();
        enabled2_ = enabled3_ = false;
        check2_ = check3_ = retry1_ = retry2_ = retry3_ = 0;
        awaitAdvance_ = anchors_.l3;
    }
    const bool waiting = anchors_.l3 && awaitAdvance_ && (changed || !haveIndex_ || !haveIndex || index == index_);
    if (awaitAdvance_ && !waiting)
        awaitAdvance_ = false;
    server_ = srv;
    head_ = head;
    root_ = root;
    mapSize_ = size;
    uber_ = uber;
    bw_ = bw;
    links_ = links;
    index_ = index;
    haveIndex_ = haveIndex;
    std::map<std::string, double> values;
    if (now < retry1_) {
        out.setText(kPcStreamL1Note, "named stats retry pending", now);
        return;
    }
    if (!named(values)) {
        retry1_ = now + 10;
        out.setText(kPcStreamL1Note, "named key/read mismatch", now);
        return;
    }
    const std::pair<const char*, uint16_t> namedIds[] = {{"sPingMS", kPcStreamNamedPing},
                                                         {"dbgEncTimeMS", kPcStreamNamedEncode},
                                                         {"txTimeMS", kPcStreamNamedSend},
                                                         {"dEstLinkBW", kPcStreamEstimatedBandwidth},
                                                         {"rexmitReq", kPcStreamRetransmitRequests},
                                                         {"rexmitSent", kPcStreamRetransmitSent},
                                                         {"nFramesSkippedPS", kPcStreamSkipped},
                                                         {"dFrmsLate", kPcStreamLate},
                                                         {"encResets", kPcStreamEncoderResets}};
    std::string missing;
    unsigned more = 0;
    for (auto kv : namedIds) {
        auto v = values.find(kv.first);
        if (v != values.end() && v->second >= 0 && v->second <= 1e12)
            out.setF32(kv.second, v->second, now);
        else if (v == values.end() && missing.empty())
            missing = std::string("missing ") + kv.first;
        else if (v == values.end())
            ++more;
    }
    if (more)
        missing += " +" + std::to_string(more);
    const bool live = values.count("sRXpps") && values["sRXpps"] > 0;
    // Once streaming, every shown key should exist: name the missing ones instead of leaving rows silently N/A.
    out.setText(kPcStreamL1Note, live ? missing : "", now);
    if (!live) {
        naRange(out, 1030, 1067, Reason::NoFrames, now);
        frames_.clear();
        enabled2_ = enabled3_ = false;
        out.setText(kPcStreamL2Note, "no frames", now);
        out.setText(kPcStreamL3Note, "no frames", now);
        return;
    }
    if (anchors_.l2 && now >= retry2_) {
        bool valid = haveLinks && rttiIs(memory_, uber, module_, anchors_.imageSize, ".?AVSVLDataLinkUber@@");
        struct Link {
            double ping, pps;
            uint64_t object;
        };
        std::vector<Link> active;
        double sum = 0;
        for (auto obj : links)
            if (obj) {
                if (!rttiIs(memory_, obj, module_, anchors_.imageSize, ".?AVSVLDataLinkTransferUDP@@")) {
                    valid = false;
                    break;
                }
                uint32_t pps = 0, ping = 0;
                if (!memory_.get(obj + 0x24, pps) || !memory_.get(obj + anchors_.linkPingOffset, ping) ||
                    ping > 500000 || pps > 10000000) {
                    valid = false;
                    break;
                }
                sum += pps;
                if (pps)
                    active.push_back({ping / 1000.0, static_cast<double>(pps), obj});
            }
        if (!enabled2_ || now >= check2_) {
            valid = valid && std::abs(sum - values["sRXpps"]) <= std::max(1.0, values["sRXpps"] * 0.1);
            check2_ = now + 10;
        }
        if (valid) {
            enabled2_ = true;
            out.setText(kPcStreamL2Note, "", now);
            naRange(out, 1060, 1067, Reason::NotConnected, now);
            for (size_t j = 0; j < std::min<size_t>(active.size(), 4); ++j) {
                out.setF32(static_cast<uint16_t>(1060 + j * 2), active[j].ping, now);
                out.setF32(static_cast<uint16_t>(1061 + j * 2), 100 * active[j].pps / sum, now);
                const auto endpoint = readLinkEndpoint(memory_, active[j].object, module_, anchors_);
                if (!endpoint.empty())
                    out.setText(static_cast<uint16_t>(1090 + j), endpoint, now);
            }
        } else {
            enabled2_ = false;
            retry2_ = now + 10;
            out.setText(kPcStreamL2Note, "link RTTI/packets mismatch", now);
        }
    } else if (anchors_.l2)
        out.setText(kPcStreamL2Note, "link validation retry pending", now);
    if (!anchors_.l3 || now < retry3_) {
        if (anchors_.l3)
            out.setText(kPcStreamL3Note, "frame validation retry pending", now);
        return;
    }
    if (waiting) {
        naRange(out, 1030, 1059, Reason::NoFrames, now);
        frames_.clear();
        enabled3_ = false;
        out.setText(kPcStreamL3Note, "stream reset; waiting", now);
        return;
    }
    if (!haveIndex || !haveBw) {
        retry3_ = now + 10;
        enabled3_ = false;
        out.setText(kPcStreamL3Note, "ring owner/index mismatch", now);
        return;
    }
    std::vector<uint8_t> tail(kTailFrames * kFrameStride);
    const size_t start = (index - kTailFrames) & 0xfff, first = std::min(kTailFrames, kFrameSlots - start);
    if (!memory_.read(srv + anchors_.aftmOffset + start * kFrameStride, tail.data(), first * kFrameStride) ||
        (first < kTailFrames && !memory_.read(srv + anchors_.aftmOffset, tail.data() + first * kFrameStride,
                                              (kTailFrames - first) * kFrameStride))) {
        retry3_ = now + 10;
        enabled3_ = false;
        frames_.clear();
        out.setText(kPcStreamL3Note, "frame read failed", now);
        return;
    }
    frames_.add(tail, now);
    auto window = frames_.recent(now);
    if (window.empty()) {
        naRange(out, 1030, 1059, Reason::NoFrames, now);
        out.setText(kPcStreamL3Note, "no frames", now);
        return;
    }
    uint32_t count = 0, bi = 0;
    if (!memory_.get(srv + 0x1e8, count) || !memory_.get(srv + 0x1ec, bi) || count != 8192 || bi >= count) {
        retry3_ = now + 10;
        enabled3_ = false;
        out.setText(kPcStreamL3Note, "bandwidth layout mismatch", now);
        return;
    }
    constexpr size_t n = 144, stride = 0xa8;
    std::vector<uint8_t> bt(n * stride);
    const size_t bs = (bi + count - n) % count, bfirst = std::min(n, count - bs);
    if (!memory_.read(bw + bs * stride, bt.data(), bfirst * stride) ||
        (bfirst < n && !memory_.read(bw, bt.data() + bfirst * stride, (n - bfirst) * stride))) {
        retry3_ = now + 10;
        enabled3_ = false;
        out.setText(kPcStreamL3Note, "bandwidth read failed", now);
        return;
    }
    const uint64_t newest = at<uint64_t>(bt.data(), (n - 1) * stride + 0x40);
    std::array<uint64_t, 7> bytes{};
    unsigned rate = 0;
    bool sane = true;
    for (size_t j = 0; j < n; ++j) {
        const auto* b = bt.data() + j * stride;
        const auto ts = at<uint64_t>(b, 0x40);
        if (!ts || ts > newest || newest - ts >= 1000000)
            continue;
        const int32_t wire = at<int32_t>(b, 0);
        if (wire < 1024 || wire > 2 * 1024 * 1024) {
            sane = false;
            break;
        }
        // At idle bitrate one record's audio (dword 4) can exceed its wire bytes (seen live: 8960 vs 8272), so audio
        // only gets the absolute bound.
        for (size_t k = 0; k < 7; ++k) {
            const auto v = at<int32_t>(b, k * 4);
            if (v < 0 || v > (k == 4 ? 2 * 1024 * 1024 : wire))
                sane = false;
            bytes[k] += static_cast<uint32_t>(std::max(0, v));
        }
        ++rate;
    }
    std::array<std::vector<double>, 9> stages;
    std::vector<double> pings;
    for (const auto& f : window) {
        for (size_t j = 0; j < (f.split ? 9 : 7); ++j)
            stages[j].push_back(f.ms[j]);
        pings.push_back(f.ping);
    }
    bool valid = sane && rate > 0;
    if (!enabled3_ || now >= check3_) {
        valid = valid && values.count("sPingMS") && values.count("dbgEncTimeMS") &&
                std::abs(average(pings) - values["sPingMS"]) <= 0.5 &&
                std::abs(average(stages[1]) - values["dbgEncTimeMS"]) <= 1.0;
        valid = valid && std::abs(static_cast<double>(rate) - window.size()) <= std::max(2.0, rate * 0.1);
        // During startup the window may contain fewer than a full second. Validate Hz once it is full.
        if (displayHz > 0 && window.size() >= displayHz * 0.8)
            valid = valid && std::abs(rate - displayHz) <= std::max(2.0, displayHz * 0.1);
        check3_ = now + 10;
    }
    if (!valid) {
        enabled3_ = false;
        retry3_ = now + 10;
        frames_.clear();
        out.setText(kPcStreamL3Note, "frame/bandwidth cross-check", now);
        return;
    }
    enabled3_ = true;
    out.setText(kPcStreamL3Note, "", now);
    out.setF32(kPcStreamFps, rate, now);
    const uint16_t bandwidthIds[] = {kPcStreamWireMbps,  kPcStreamVideoMbps, kPcStreamFecMbps,
                                     kPcStreamLogicMbps, kPcStreamAudioMbps, kPcStreamRetryMbps};
    for (unsigned j = 0; j < 6; ++j)
        out.setF32(bandwidthIds[j], bytes[j] * 8.0 / 1048576.0, now);
    for (unsigned j = 0; j < 9; ++j) {
        if (stages[j].empty()) {
            naRange(out, static_cast<uint16_t>(1040 + 2 * j), static_cast<uint16_t>(1041 + 2 * j), Reason::NoFrames,
                    now);
            continue;
        }
        out.setF32(static_cast<uint16_t>(1040 + 2 * j), average(stages[j]), now);
        out.setF32(static_cast<uint16_t>(1041 + 2 * j), p95(stages[j]), now);
    }
}
bool parseLogEvent(const std::string& line, LogEvents& e) {
    auto increment = [](uint32_t& n) {
        if (n < static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            ++n;
    };
    if (line.find("Reset video stream") != std::string::npos)
        increment(e.resets);
    else if (line.find("Bad link event") != std::string::npos)
        increment(e.badLinks);
    else if (line.find("Finished encoder reinit") != std::string::npos)
        increment(e.encoderReinits);
    else if (line.find("THROTTLE EVENT") != std::string::npos)
        increment(e.throttles);
    else
        return false;
    return true;
}
void LogParser::feed(const char* data, size_t size) {
    for (size_t j = 0; j < size; ++j) {
        const char c = data[j];
        if (c == '\n') {
            if (!dropping_)
                parseLogEvent(partial_, events_);
            partial_.clear();
            dropping_ = false;
        } else if (!dropping_) {
            if (partial_.size() < 4096)
                partial_ += c;
            else {
                partial_.clear();
                dropping_ = true;
            }
        }
    }
}
void LogParser::rewritten() {
    partial_.clear();
    dropping_ = false;
}
} // namespace streaming
} // namespace pc
} // namespace fph
