#include "reader.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
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
const FrameLayout kFrameLayouts[2] = {
    {"legacy", kCsvHeader, kCsvRow, 0x78, 0x88030, 0x88000,
     {0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30}, 0x38},
    {"gpu-stamps",
     "Present ID,WGP Release,App Submit,VrCompositor GPU Done,VrLink GPU Done,Encode Start,Encode End,Send "
     "Start,Send End,Receive,Decode Start,Decode End,Submit Illumination,Predicted Display,Decode Query,Run Start "
     "At WGP Degate,Ping At Send Start,Filter Ping At Send Start,Upstream Ping At Send Start,WGP Release "
     "(realtime),Receive (remote realtime),Receive (remote),Decode Start (remote),Decode End (remote),Submit "
     "Illumination (remote),Decode Query (remote),TX ID,Problem Flags\n",
     "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d,%llu,%llu,%u,%u,%u,%u,%u,%d,%llx\n",
     0x98, 0xa8080, 0xa8050,
     {0x14, 0x18, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x48}, 0x50}};
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
    uint32_t timestamp = 0, imageSize = 0, text = 0, textSize = 0, textRva = 0, dataRva = 0, dataSize = 0;
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
            if (std::memcmp(b.data() + s, ".data\0\0\0", 8) == 0) {
                dataRva = v.rva;
                dataSize = v.size;
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
    // x64 RTTI: type descriptor (name - 16) -> complete object locator (signature 1, self RVA) -> vtable after the
    // locator pointer. Returns the raw file offsets of each vtable.
    std::vector<size_t> vtables(const char* name) const {
        std::vector<size_t> out;
        for (const auto str : strings(name)) {
            if (str < 16)
                continue;
            const uint32_t descriptor = str - 16;
            for (size_t i = 12; i + 12 <= b.size(); ++i) {
                if (at<uint32_t>(b.data(), i) != descriptor)
                    continue;
                const size_t col = i - 12;
                const uint32_t colRva = rva(col);
                if (!colRva || at<uint32_t>(b.data(), col) != 1 || at<uint32_t>(b.data(), col + 4) != 0 ||
                    at<uint32_t>(b.data(), col + 20) != colRva)
                    continue;
                const uint64_t locator = imageBase + colRva;
                for (size_t j = 0; j + 16 <= b.size(); ++j)
                    if (at<uint64_t>(b.data(), j) == locator && rva(j + 8))
                        out.push_back(j + 8);
            }
        }
        return out;
    }
};
// Reads NUL-terminated strings starting with `prefix` (preceded by a NUL) and ending in '\n'.
std::vector<std::pair<uint32_t, std::string>> csvStrings(const Pe& p, const char* prefix) {
    std::vector<std::pair<uint32_t, std::string>> out;
    const auto& b = p.b;
    const size_t n = std::strlen(prefix);
    for (size_t i = 1; i + n < b.size(); ++i) {
        if (b[i - 1] != 0 || std::memcmp(b.data() + i, prefix, n) != 0)
            continue;
        size_t e = i;
        while (e < b.size() && b[e] && e - i < 4096)
            ++e;
        if (e < b.size() && !b[e] && e > i && b[e - 1] == '\n' && p.rva(i))
            out.push_back({p.rva(i), std::string(reinterpret_cast<const char*>(b.data() + i), e - i)});
    }
    return out;
}
std::vector<std::string> csvFields(std::string s) {
    if (!s.empty() && s.back() == '\n')
        s.pop_back();
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t comma; (comma = s.find(',', start)) != std::string::npos; start = comma + 1)
        out.push_back(s.substr(start, comma - start));
    out.push_back(s.substr(start));
    return out;
}
struct ModRm {
    unsigned mod = 0, reg = 0, rm = 0; // rm is the base register for a memory operand
    int32_t disp = 0;
    size_t length = 0;          // ModRM, SIB and displacement bytes
    bool memory = false, simple = false; // simple: [base+disp], no index, not RIP-relative
};
bool modRm(const uint8_t* b, size_t size, size_t i, unsigned rex, ModRm& m) {
    if (i >= size)
        return false;
    const uint8_t x = b[i];
    m = {};
    m.mod = x >> 6;
    m.reg = ((x >> 3) & 7) | (rex & 4 ? 8 : 0);
    m.rm = (x & 7) | (rex & 1 ? 8 : 0);
    m.length = 1;
    m.memory = m.mod != 3;
    m.simple = m.memory;
    if (!m.memory)
        return true;
    bool disp32 = m.mod == 2;
    if ((x & 7) == 4) {
        if (i + 1 >= size)
            return false;
        const uint8_t sib = b[i + 1];
        m.length = 2;
        if ((((sib >> 3) & 7) | (rex & 2 ? 8 : 0)) != 4)
            m.simple = false;
        m.rm = (sib & 7) | (rex & 1 ? 8 : 0);
        if (m.mod == 0 && (sib & 7) == 5) {
            m.simple = false;
            disp32 = true;
        }
    } else if (m.mod == 0 && (x & 7) == 5) {
        m.simple = false;
        disp32 = true;
    }
    const size_t n = disp32 ? 4 : m.mod == 1 ? 1 : 0;
    if (i + m.length + n > size)
        return false;
    if (n == 1)
        m.disp = static_cast<int8_t>(b[i + m.length]);
    else if (n == 4)
        m.disp = at<int32_t>(b, i + m.length);
    m.length += n;
    return true;
}
// One pass over the writer. `print` is the offset of the call that prints the row (unknown on the first pass, which
// only reports where that call is in `printAt`).
bool decodePass(const uint8_t* b, size_t size, size_t lea, size_t print, WriterShape& out, size_t& printAt) {
    out = {};
    printAt = 0;
    std::array<int32_t, 16> gpr, xmm;
    gpr.fill(-1);
    xmm.fill(-1);
    std::map<std::pair<unsigned, int32_t>, int32_t> slots; // (base register, displacement) byte -> record offset
    std::map<int32_t, int32_t> stack;                      // printf argument 2+k at [rsp+0x20+8k] -> record offset
    int record = -1;
    bool conflict = false, done = false;
    const auto fromSlot = [&](unsigned base, int32_t disp, unsigned n) {
        const auto first = slots.find({base, disp});
        if (first == slots.end() || first->second < 0)
            return -1;
        for (unsigned k = 1; k < n; ++k) {
            const auto s = slots.find({base, disp + static_cast<int32_t>(k)});
            if (s == slots.end() || s->second != first->second + static_cast<int32_t>(k))
                return -1;
        }
        return first->second;
    };
    const auto setSlots = [&](unsigned base, int32_t disp, unsigned n, int32_t v) {
        for (unsigned k = 0; k < n; ++k)
            slots[{base, disp + static_cast<int32_t>(k)}] = v < 0 ? -1 : v + static_cast<int32_t>(k);
    };
    const auto write = [&](unsigned r) {
        gpr[r] = -1;
        if (static_cast<int>(r) == record)
            record = -1;
        slots.erase(slots.lower_bound({r, std::numeric_limits<int32_t>::min()}),
                    slots.lower_bound({r + 1, std::numeric_limits<int32_t>::min()}));
    };
    // Only the supported subset below is decoded; anything else fails the derivation, since an unknown instruction
    // could change a value that still looks like a plain record field. Code a forward branch can skip may not run, so
    // what it writes loses its provenance. Only a branch past the print call is an early-out that skips the row.
    size_t skippable = 0;
    const auto branch = [&](size_t next, int32_t rel) {
        const int64_t target = static_cast<int64_t>(next) + rel;
        if (target <= static_cast<int64_t>(next) - 1)
            return false; // a loop: not this writer's shape
        if (target <= static_cast<int64_t>(print))
            skippable = std::max(skippable, static_cast<size_t>(target));
        return true;
    };
    for (size_t i = 0; i < size && !done;) {
        const bool passed = i > lea, cond = i < skippable;
        const auto known = [&](int32_t v) { return cond ? -1 : v; };
        unsigned rex = 0, rep = 0;
        bool op16 = false;
        while (i < size && (b[i] == 0x66 || b[i] == 0xf2 || b[i] == 0xf3)) {
            op16 = op16 || b[i] == 0x66;
            rep = b[i] == 0x66 ? rep : b[i];
            ++i;
        }
        if (i < size && (b[i] & 0xf0) == 0x40)
            rex = b[i++];
        if (i >= size)
            break;
        const uint8_t op = b[i];
        const unsigned width = rex & 8 ? 8 : op16 ? 2 : 4;
        ModRm m;
        if (op == 0x0f) {
            if (i + 1 >= size)
                break;
            const uint8_t op2 = b[i + 1];
            if (op2 >= 0x80 && op2 <= 0x8f) {
                if (i + 6 > size || !branch(i + 6, at<int32_t>(b, i + 2)))
                    return false;
                i += 6;
                continue;
            }
            const bool move = op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29;
            const bool other =
                op2 == 0x1f || op2 == 0x57 || op2 == 0xaf || op2 == 0xb6 || op2 == 0xb7 || op2 == 0xbe || op2 == 0xbf;
            if ((!move && !other) || !modRm(b, size, i + 2, rex, m))
                return false;
            i += 2 + m.length;
            if (move) {
                const unsigned n = rep == 0xf3 ? 4 : rep == 0xf2 ? 8 : 16;
                if (op2 == 0x10 || op2 == 0x28)
                    xmm[m.reg] = known(!m.memory                             ? xmm[m.rm]
                                       : !m.simple                           ? -1
                                       : static_cast<int>(m.rm) == record ? m.disp
                                                                             : fromSlot(m.rm, m.disp, n));
                else if (!m.memory)
                    xmm[m.rm] = known(xmm[m.reg]);
                else if (m.simple)
                    setSlots(m.rm, m.disp, n, known(xmm[m.reg]));
                else
                    return false; // an indexed store: can't tell which slots it changes
            } else if (op2 == 0x57)
                xmm[m.reg] = -1;
            else if (op2 != 0x1f)
                write(m.reg);
            continue;
        }
        switch (op) {
        case 0x8b: { // mov r, r/m
            if (!modRm(b, size, i + 1, rex, m))
                return false;
            i += 1 + m.length;
            int32_t v = -1;
            if (!m.memory)
                v = gpr[m.rm];
            else if (m.simple && static_cast<int>(m.rm) == record)
                v = m.disp;
            else if (m.simple) {
                v = fromSlot(m.rm, m.disp, width);
                // The FILE* the row goes to: mov rcx,[owner+disp] right before the call.
                if (v < 0 && m.reg == 1 && width == 8 && m.rm != 4 && m.rm != 5 && m.disp >= 0x1000 && !cond)
                    out.fileOffset = static_cast<uint32_t>(m.disp);
            }
            write(m.reg);
            gpr[m.reg] = known(v);
            continue;
        }
        case 0x89: { // mov r/m, r
            if (!modRm(b, size, i + 1, rex, m))
                return false;
            i += 1 + m.length;
            if (!m.memory) {
                const auto v = known(gpr[m.reg]);
                write(m.rm);
                gpr[m.rm] = v;
            } else if (m.simple && m.rm == 4) {
                if (m.disp >= 0x20 && (m.disp - 0x20) % 8 == 0)
                    stack[(m.disp - 0x20) / 8] = known(gpr[m.reg]);
            } else if (m.simple)
                setSlots(m.rm, m.disp, width, known(gpr[m.reg]));
            else
                return false;
            continue;
        }
        case 0x69:
        case 0x6b: { // imul r, r/m, imm: the ring stride, and the register that becomes the record pointer
            if (!modRm(b, size, i + 1, rex, m))
                return false;
            const size_t immAt = i + 1 + m.length, n = op == 0x69 ? 4 : 1;
            if (immAt + n > size)
                return false;
            const int32_t imm = n == 4 ? at<int32_t>(b, immAt) : static_cast<int8_t>(b[immAt]);
            i = immAt + n;
            write(m.reg);
            if (!m.memory && imm > 0 && !cond) {
                conflict = conflict || (out.stride && out.stride != static_cast<uint32_t>(imm));
                out.stride = static_cast<uint32_t>(imm);
                record = static_cast<int>(m.reg);
            }
            continue;
        }
        case 0x01:
        case 0x03: // add: adding the ring base to the record register keeps it the record pointer
        case 0x09:
        case 0x0b:
        case 0x21:
        case 0x23:
        case 0x29:
        case 0x2b:
        case 0x31:
        case 0x33:
        case 0x63:
        case 0x8d:
        case 0x88:
        case 0x8a:
        case 0x38:
        case 0x39:
        case 0x3a:
        case 0x3b:
        case 0x84:
        case 0x85: {
            if (!modRm(b, size, i + 1, rex, m))
                return false;
            i += 1 + m.length;
            const bool compare = (op >= 0x38 && op <= 0x3b) || op == 0x84 || op == 0x85;
            const bool toRm = op == 0x01 || op == 0x09 || op == 0x21 || op == 0x29 || op == 0x31 || op == 0x88;
            const unsigned dest = toRm ? m.rm : m.reg;
            const bool keep = (op == 0x03 || op == 0x01) && !m.memory && static_cast<int>(dest) == record && !cond;
            if (compare || keep)
                continue;
            if (toRm && m.memory) {
                if (!m.simple)
                    return false;
                setSlots(m.rm, m.disp, width, -1);
            } else
                write(dest);
            continue;
        }
        case 0x80:
        case 0x81:
        case 0x83:
        case 0xc6:
        case 0xc7: {
            if (!modRm(b, size, i + 1, rex, m))
                return false;
            const size_t n = op == 0x81 || op == 0xc7 ? (op16 ? 2 : 4) : 1;
            i += 1 + m.length + n;
            if (m.memory) {
                const bool writes = op == 0xc6 || op == 0xc7 || (m.reg & 7) != 7;
                if (writes && !m.simple)
                    return false;
                if (writes)
                    setSlots(m.rm, m.disp, op == 0xc6 || op == 0x80 ? 1 : width, -1);
            } else if (op == 0xc6 || op == 0xc7 || (m.reg & 7) != 7)
                write(m.rm);
            continue;
        }
        case 0xe8:
            if (passed) {
                done = true;
                printAt = i;
                break;
            }
            for (unsigned r : {0u, 1u, 2u, 8u, 9u, 10u, 11u})
                write(r);
            for (unsigned r = 0; r < 6; ++r)
                xmm[r] = -1;
            i += 5;
            continue;
        case 0xe9:
            if (i + 5 > size || !branch(i + 5, at<int32_t>(b, i + 1)))
                return false;
            i += 5;
            continue;
        case 0xeb:
            if (i + 2 > size || !branch(i + 2, static_cast<int8_t>(b[i + 1])))
                return false;
            i += 2;
            continue;
        case 0x90:
            ++i;
            continue;
        default:
            if (op >= 0x70 && op <= 0x7f) {
                if (i + 2 > size || !branch(i + 2, static_cast<int8_t>(b[i + 1])))
                    return false;
                i += 2;
            } else if (op >= 0x50 && op <= 0x57)
                ++i;
            else if (op >= 0x58 && op <= 0x5f) {
                write((op & 7) | (rex & 1 ? 8 : 0));
                ++i;
            } else if (op >= 0xb8 && op <= 0xbf) {
                write((op & 7) | (rex & 1 ? 8 : 0));
                i += 1 + (rex & 8 ? 8 : 4);
            } else
                return false;
            continue;
        }
    }
    if (!done || conflict || !out.stride)
        return false;
    int32_t last = -1;
    for (const auto& kv : stack)
        last = std::max(last, kv.first);
    out.args = {gpr[8], gpr[9]};
    for (int32_t k = 0; k <= last; ++k) {
        const auto s = stack.find(k);
        out.args.push_back(s == stack.end() ? -1 : s->second);
    }
    return true;
}
} // namespace
bool decodeCsvWriter(const uint8_t* b, size_t size, size_t lea, WriterShape& out) {
    // First find the print call (no branch skips anything yet), then decode knowing which branches jump past it.
    size_t print = 0;
    decodePass(b, size, lea, 0, out, print);
    return print && decodePass(b, size, lea, print, out, print);
}
bool deriveLayout(const std::string& header, const std::string& row, const WriterShape& w, DerivedLayout& out,
                  std::string& note) {
    note.clear();
    const auto columns = csvFields(header), specs = csvFields(row);
    if (columns.size() != specs.size() || columns.size() < 2) {
        note = "CSV header/row count mismatch";
        return false;
    }
    if (!w.stride || w.stride > 0x400) {
        note = "CSV writer stride not found";
        return false;
    }
    const auto field = [&](const char* name, uint32_t& offset, bool optional) {
        size_t found = columns.size();
        for (size_t j = 0; j < columns.size(); ++j)
            if (columns[j] == name) {
                if (found != columns.size()) {
                    note = std::string(name) + " twice";
                    return false;
                }
                found = j;
            }
        if (found == columns.size()) {
            if (!optional)
                note = std::string("no column ") + name;
            return false;
        }
        if (specs[found] != "%u" && specs[found] != "%d") {
            note = std::string(name) + " not 32-bit";
            return false;
        }
        if (found >= w.args.size() || w.args[found] < 0 || static_cast<uint32_t>(w.args[found]) + 4 > w.stride) {
            note = std::string(name) + " no offset";
            return false;
        }
        offset = static_cast<uint32_t>(w.args[found]);
        return true;
    };
    static const char* const stamps[12] = {"WGP Release",  "App Submit", "Encode Start",        "Encode End",
                                           "Send Start",   "Send End",   "Receive",             "Decode Start",
                                           "Decode End",   "Submit Illumination", "Predicted Display", "Decode Query"};
    uint32_t id = 0;
    if (!field("Present ID", id, false))
        return false;
    if (id != 0) {
        note = "Present ID not at record start";
        return false;
    }
    FrameLayout& l = out.layout;
    for (size_t j = 0; j < 12; ++j)
        if (j != 10 && !field(stamps[j], l.stamps[j], false))
            return false;
    // parseFrame never reads Predicted Display, so a build without it still works.
    if (!field(stamps[10], l.stamps[10], true)) {
        if (!note.empty())
            return false;
        l.stamps[10] = l.stamps[11];
    }
    if (!field("Ping At Send Start", l.pingOffset, false))
        return false;
    out.header = header;
    out.row = row;
    l.name = "derived";
    l.header = out.header.c_str();
    l.row = out.row.c_str();
    l.stride = w.stride;
    l.fileOffset = w.fileOffset;
    l.indexOffset = 0;
    note.clear();
    return true;
}
namespace {
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
Anchors findAnchors(const std::vector<uint8_t>& dll, bool forceDerive) {
    Anchors a;
    a.layout = nullptr;
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
    // The server global's store pair. The gap between the two globals is a linker accident (0x40, then -0xa18 on
    // build 1794602179), so only the global's place in .data is checked.
    for (size_t i = p.text; i + 14 <= p.text + p.textSize; ++i) {
        if (std::memcmp(b + i, "\x4c\x89\x3d", 3) == 0 && std::memcmp(b + i + 7, "\x48\x89\x1d", 3) == 0) {
            const int64_t g = static_cast<int64_t>(p.rva(i)) + 7 + at<int32_t>(b, i + 3);
            if (g > 0 && g + 8 < a.imageSize && (!p.dataSize || (g >= p.dataRva && g + 8 <= p.dataRva + p.dataSize)))
                globals.insert(static_cast<uint32_t>(g));
        }
    }
    for (const auto vt : p.vtables(".?AVSVLServer@@"))
        a.serverVtables.push_back(p.rva(vt));
    a.dataRva = p.dataRva;
    a.dataSize = p.dataSize;
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
    size_t headers = 0;
    for (const auto& layout : kFrameLayouts) {
        if (forceDerive)
            break;
        const auto matches = p.strings(layout.header).size();
        headers += matches;
        if (matches == 1)
            a.layout = &layout;
    }
    if (headers != 1)
        a.layout = nullptr;
    std::string deriveNote;
    if (!headers) {
        // Unknown header: read the layout off the CSV writer, matching columns by name.
        const auto heads = csvStrings(p, "Present ID,");
        std::vector<std::shared_ptr<DerivedLayout>> found;
        if (heads.size() != 1)
            deriveNote = heads.empty() ? "CSV header not found" : "CSV header ambiguous";
        else {
            const auto columns = csvFields(heads[0].second).size();
            for (const auto& row : csvStrings(p, "%"))
                if (csvFields(row.second).size() == columns)
                    for (auto ref : p.refs(row.first)) {
                        // Function start: the nearest 16-aligned address after int3/ret padding.
                        size_t start = ref;
                        const size_t floor = ref > p.text + 0x600 ? ref - 0x600 : p.text;
                        while (start > floor && !(p.rva(start) % 16 == 0 && (b[start - 1] == 0xcc || b[start - 1] == 0xc3)))
                            --start;
                        WriterShape shape;
                        auto d = std::make_shared<DerivedLayout>();
                        std::string note;
                        const size_t end = std::min<size_t>(p.text + p.textSize, ref + 0x400);
                        if (start <= floor || !decodeCsvWriter(b + start, end - start, ref - start, shape))
                            note = "CSV writer shape mismatch";
                        else if (deriveLayout(heads[0].second, row.second, shape, *d, note))
                            found.push_back(d);
                        if (deriveNote.empty())
                            deriveNote = note;
                    }
            if (found.empty() && deriveNote.empty())
                deriveNote = "CSV writer not found";
            for (const auto& d : found)
                if (d->layout.stride != found[0]->layout.stride || d->layout.stamps != found[0]->layout.stamps ||
                    d->layout.pingOffset != found[0]->layout.pingOffset || d->row != found[0]->row) {
                    found.clear();
                    deriveNote = "CSV writers disagree";
                    break;
                }
        }
        if (!found.empty()) {
            a.derived = found[0];
            a.layout = &found[0]->layout;
            a.stride = a.layout->stride;
            a.fileOffset = a.layout->fileOffset;
            a.layoutHow = "derived";
        } else
            a.layoutHow = "derive failed";
    } else if (a.layout)
        a.layoutHow = std::string("known ") + a.layout->name;
    else
        a.layoutHow = "known headers ambiguous";
    if (a.layout && !a.derived) {
        for (auto str : p.strings(a.layout->row))
            for (auto ref : p.refs(str)) {
                const size_t begin = ref >= p.text + 0x1a0 ? ref - 0x1a0 : p.text;
                const size_t end = std::min(ref + 0x20, static_cast<size_t>(p.text + p.textSize));
                for (size_t j = begin; j + 7 <= end; ++j) {
                    if (j + 7 <= ref && std::memcmp(b + j, "\x48\x6b\xd0", 3) == 0)
                        strides.insert(b[j + 3]);
                    if (j + 7 <= ref && std::memcmp(b + j, "\x48\x69\xf8", 3) == 0)
                        strides.insert(at<uint32_t>(b, j + 3));
                    if ((j + 7 <= ref && std::memcmp(b + j, "\x4c\x8b\x91", 3) == 0) ||
                        (j >= ref && std::memcmp(b + j, "\x48\x8b\x8e", 3) == 0))
                        files.insert(at<uint32_t>(b, j + 3));
                }
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
    if (strides.size() == 1 && !a.derived)
        a.stride = *strides.begin();
    if (files.size() == 1 && !a.derived)
        a.fileOffset = *files.begin();
    const bool scan = !a.serverVtables.empty() && a.dataSize;
    a.serverHow = a.serverGlobal && scan ? "code pair+RTTI scan" : a.serverGlobal ? "code pair" : scan ? "RTTI scan" : "none";
    a.l1 = (a.serverGlobal || scan) && a.mapOffset && p.strings(".?AVSVLServer@@").size() == 1;
    a.l2 = a.linkPingOffset && a.uberPingOffset && p.strings(".?AVSVLDataLinkUber@@").size() == 1 &&
           p.strings(".?AVSVLDataLinkTransferUDP@@").size() == 1;
    a.l3 = a.layout && a.aftmOffset && a.stride == a.layout->stride && a.fileOffset == a.layout->fileOffset;
    // Locate the primary UDP vtable via x64 RTTI. Its remote-address getter (slot 0x50)
    // takes a sockaddr from this+offset, checks AF_INET/AF_INET6, then formats it.
    std::set<std::pair<uint32_t, uint32_t>> endpoints;
    for (const auto vt : p.vtables(".?AVSVLDataLinkTransferUDP@@")) {
        if (vt + 0x58 > dll.size())
            continue;
        const uint64_t address = at<uint64_t>(b, vt + 0x50);
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
    else if (!deriveNote.empty())
        a.l3Note = deriveNote;
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
bool parseFrame(const uint8_t* b, Frame& f, const FrameLayout& layout) {
    uint32_t t[13];
    t[0] = at<uint32_t>(b, 0);
    for (size_t j = 0; j < layout.stamps.size(); ++j)
        t[j + 1] = at<uint32_t>(b, layout.stamps[j]);
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
    const int32_t ping = at<int32_t>(b, layout.pingOffset);
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
void FrameWindow::add(const std::vector<uint8_t>& tail, double now, const FrameLayout& layout) {
    if (tail.size() != kTailFrames * layout.stride)
        return;
    // Completion order can differ from record order. An older record finishing late must never move
    // the cutoff backwards, including when the u32 illumination clock wraps.
    for (size_t j = 0; j < kTailFrames; ++j) {
        Frame f;
        if (!parseFrame(tail.data() + j * layout.stride, f, layout))
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
        if (!parseFrame(tail.data() + j * layout.stride, f, layout) ||
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
uint32_t Reader::indexOffset() const {
    const auto* layout = anchors_.layout;
    return !layout ? 0 : layout->indexOffset ? layout->indexOffset : foundIndex_;
}
bool Reader::readLarge(uint64_t address, uint8_t* dest, size_t bytes) {
    for (size_t done = 0; done < bytes;) {
        const size_t n = std::min<size_t>(bytes - done, 0x100000);
        if (!memory_.read(address + done, dest + done, n))
            return false;
        done += n;
    }
    return true;
}
bool Reader::serverAt(uint32_t global, uint64_t& srv) {
    return global && memory_.get(module_ + global, srv) &&
           rttiIs(memory_, srv, module_, anchors_.imageSize, ".?AVSVLServer@@");
}
bool Reader::locateServer(double now, uint64_t& srv) {
    // Only a failed RTTI check rescans: a chosen global still holding an intact old server while another global moved
    // on is not caught here (SVLServer lifetime across HMD re-activation is unverified).
    if (serverAt(global_, srv))
        return true;
    global_ = 0;
    serverHow_.clear();
    uint64_t code = 0;
    const bool haveCode = serverAt(anchors_.serverGlobal, code);
    // Every .data qword that points at an SVLServer (six globals on build 1794602179, all the same object). Only
    // at attach or after the global stops validating, at most every 10 s: the object is absent until the HMD
    // activates.
    std::set<uint64_t> objects;
    uint32_t first = 0;
    bool scanned = false; // a scan ran and its .data read succeeded
    if (!anchors_.serverVtables.empty() && anchors_.dataSize >= 8 && now >= scanAt_) {
        scanAt_ = now + 10;
        std::vector<uint8_t> data(anchors_.dataSize & ~7u);
        if (readLarge(module_ + anchors_.dataRva, data.data(), data.size())) {
            scanned = true;
            const std::set<uint32_t> vtables(anchors_.serverVtables.begin(), anchors_.serverVtables.end());
            std::map<uint64_t, bool> checked;
            for (size_t off = 0; off + 8 <= data.size(); off += 8) {
                const auto p = at<uint64_t>(data.data(), off);
                if (!pointer(p) || (p & 7))
                    continue;
                auto c = checked.find(p);
                if (c == checked.end()) {
                    uint64_t vt = 0;
                    const bool is = memory_.get(p, vt) && vt >= module_ && vt - module_ < anchors_.imageSize &&
                                    vtables.count(static_cast<uint32_t>(vt - module_)) &&
                                    rttiIs(memory_, p, module_, anchors_.imageSize, ".?AVSVLServer@@");
                    c = checked.emplace(p, is).first;
                }
                if (c->second) {
                    objects.insert(p);
                    if (!first)
                        first = anchors_.dataRva + static_cast<uint32_t>(off);
                }
            }
        }
    }
    // A disagreement holds until a later completed scan resolves it: between scans, or after a failed read, nothing
    // tells which server is current.
    if (scanned)
        disagree_ = objects.size() > 1 || (haveCode && !objects.empty() && *objects.begin() != code);
    if (disagree_) {
        serverNote_ = "server globals disagree";
        return false;
    }
    serverNote_.clear();
    char rva[32];
    if (haveCode) {
        global_ = anchors_.serverGlobal;
        srv = code;
        serverHow_ = objects.empty() ? "code pair" : "code pair, RTTI scan agrees";
    } else if (!objects.empty()) {
        global_ = first;
        srv = *objects.begin();
        serverHow_ = "RTTI scan";
    } else
        return false;
    std::snprintf(rva, sizeof(rva), " (rva %x)", global_);
    serverHow_ += rva;
    return true;
}
bool Reader::findIndex(uint64_t ring) {
    // The u16 after the ring that equals (slot of the newest record + 1) and advances with it. The known layouts sit
    // at ring end +0x10000 and +0x10050. Reads ~736 KiB per hunting tick at stride 0x98; derived layouts only.
    constexpr size_t kSpan = 0x20000;
    const size_t stride = anchors_.layout->stride, ringBytes = kFrameSlots * stride;
    std::vector<uint8_t> region(kSpan), records(ringBytes);
    if (!readLarge(ring + ringBytes, region.data(), kSpan) || !readLarge(ring, records.data(), ringBytes)) {
        indexCandidates_.clear();
        return false;
    }
    uint32_t newest = 0;
    size_t slot = 0;
    for (size_t j = 0; j < kFrameSlots; ++j) {
        const auto id = at<uint32_t>(records.data(), j * stride);
        if (id > newest) {
            newest = id;
            slot = j;
        }
    }
    if (!newest)
        return false;
    const auto expected = static_cast<uint16_t>((slot + 1) & 0xfff);
    // A record can land between the two reads, so allow one slot either way.
    const auto near = [&](uint16_t v) {
        return v < kFrameSlots && (((v - expected) & 0xfff) <= 1 || ((expected - v) & 0xfff) <= 1);
    };
    if (indexCandidates_.empty()) {
        for (uint32_t off = 0; off + 2 <= kSpan; off += 2) {
            const auto v = at<uint16_t>(region.data(), off);
            if (near(v))
                indexCandidates_.push_back({off, v});
        }
        candidateExpected_ = expected;
        return false;
    }
    if (expected == candidateExpected_)
        return false;
    candidateExpected_ = expected;
    std::vector<IndexCandidate> kept;
    for (const auto c : indexCandidates_) {
        const auto v = at<uint16_t>(region.data(), c.offset);
        if (near(v) && v != c.value)
            kept.push_back({c.offset, v});
    }
    indexCandidates_ = kept;
    if (kept.size() != 1)
        return false;
    foundIndex_ = static_cast<uint32_t>(ringBytes + kept[0].offset);
    indexCandidates_.clear();
    return true;
}
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
void Reader::tick(double now, MetricStore& out) {
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
    if (!anchors_.l1 || !locateServer(now, srv) || !memory_.get(srv + anchors_.mapOffset, head) || !pointer(head) ||
        !memory_.get(head + 8, root) || !memory_.get(srv + anchors_.mapOffset + 8, size)) {
        nodes_.clear();
        frames_.clear();
        out.setText(kPcStreamL1Note, serverNote_.empty() ? "server RTTI/map mismatch" : serverNote_, now);
        return;
    }
    const bool haveUber = memory_.get(srv + 0x188, uber) && pointer(uber);
    const bool haveLinks = haveUber && memory_.read(uber + 0x388, links.data(), sizeof(links));
    const bool haveBw = memory_.get(srv + 0x1f0, bw) && pointer(bw);
    const auto* layout = anchors_.layout;
    const uint32_t indexAt = indexOffset();
    const bool haveIndex = anchors_.l3 && layout && indexAt && memory_.get(srv + anchors_.aftmOffset + indexAt, index) &&
                           index < kFrameSlots;
    const unsigned advance = haveIndex_ ? (index - index_) & 0xfff : 0;
    const bool changed = server_ && (server_ != srv || head_ != head || root_ != root || uber_ != uber || bw_ != bw ||
                                     links_ != links || (haveIndex_ && haveIndex && advance > 2048));
    if (changed) {
        ++resets_;
        nodes_.clear();
        frames_.clear();
        encodeSamples_.clear();
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
        encodeSamples_.clear();
        enabled2_ = enabled3_ = false;
        out.setText(kPcStreamL2Note, "no frames", now);
        out.setText(kPcStreamL3Note, "no frames", now);
        return;
    }
    while (!encodeSamples_.empty() && now - encodeSamples_.front().first >= 5.0)
        encodeSamples_.pop_front();
    const auto encode = values.find("dbgEncTimeMS");
    if (encode != values.end())
        encodeSamples_.push_back({now, encode->second});
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
    if (!anchors_.l3 || !layout || now < retry3_) {
        if (anchors_.l3)
            out.setText(kPcStreamL3Note, "frame validation retry pending", now);
        return;
    }
    if (!indexAt) {
        naRange(out, 1030, 1059, Reason::NoFrames, now);
        frames_.clear();
        enabled3_ = false;
        out.setText(kPcStreamL3Note, findIndex(srv + anchors_.aftmOffset) ? "write index found" : "finding write index",
                    now);
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
    const size_t frameStride = layout->stride;
    std::vector<uint8_t> tail(kTailFrames * frameStride);
    const size_t start = (index - kTailFrames) & 0xfff, first = std::min(kTailFrames, kFrameSlots - start);
    if (!memory_.read(srv + anchors_.aftmOffset + start * frameStride, tail.data(), first * frameStride) ||
        (first < kTailFrames && !memory_.read(srv + anchors_.aftmOffset, tail.data() + first * frameStride,
                                              (kTailFrames - first) * frameStride))) {
        retry3_ = now + 10;
        enabled3_ = false;
        frames_.clear();
        out.setText(kPcStreamL3Note, "frame read failed", now);
        return;
    }
    frames_.add(tail, now, *layout);
    auto window = frames_.recent(now);
    if (window.empty()) {
        // A wrong runtime-found index reads a stale part of the ring: find it again.
        if (!layout->indexOffset)
            foundIndex_ = 0;
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
        // Service ticks every 0.5 s. Five seconds smooths named-encode jitter; a sustained shift can cost one retry.
        double namedEncode = 0;
        for (const auto& sample : encodeSamples_)
            namedEncode += sample.second;
        valid = valid && values.count("sPingMS") && values.count("dbgEncTimeMS") &&
                std::abs(average(pings) - values["sPingMS"]) <= 0.5 &&
                !encodeSamples_.empty() && std::abs(average(stages[1]) - namedEncode / encodeSamples_.size()) <= 1.0;
        // No HMD display-Hz term: OpenVR's display frequency lags a mid-session refresh change (72 -> 90 Hz on
        // 2026-10-29 kept L3 off for the whole session), so the frame ring is checked against the bandwidth ring only.
        valid = valid && std::abs(static_cast<double>(rate) - window.size()) <= std::max(2.0, rate * 0.1);
        check3_ = now + 10;
    }
    if (!valid) {
        enabled3_ = false;
        retry3_ = now + 10;
        frames_.clear();
        if (!layout->indexOffset)
            foundIndex_ = 0;
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
