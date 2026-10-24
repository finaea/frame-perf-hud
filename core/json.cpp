// 設定ファイル用の小さな JSON パーサの実装。再帰下降で、深さは制限つき。
#include "json.h"

#include <cstdlib>

namespace {

/**
 * 解析中の位置を持つ小さなパーサ。
 */
class Parser {
public:
    /**
     * @param source 解析する文字列
     */
    explicit Parser(const std::string& source) : src_(source) {}

    /**
     * 文書全体を解析する。
     * @param out 結果の書き込み先
     * @param error 失敗理由
     * @return 成功したら true
     */
    bool parseDocument(JsonValue& out, std::string& error) {
        skipSpace();
        if (!parseValue(out, 0)) {
            error = error_ + "（" + std::to_string(pos_) + " 文字目付近）";
            return false;
        }
        skipSpace();
        if (pos_ != src_.size()) {
            error = "値のあとに余分な文字があります（" + std::to_string(pos_) + " 文字目付近）";
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 32;

    const std::string& src_;
    size_t pos_ = 0;
    std::string error_;

    /**
     * 失敗理由を記録する。
     * @param message 理由
     * @return 常に false（return fail(...) と書くため）
     */
    bool fail(const std::string& message) {
        if (error_.empty()) error_ = message;
        return false;
    }

    /** 空白と改行を読み飛ばす。 */
    void skipSpace() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    /**
     * 指定の文字列がそのまま続くか確かめて読み進める。
     * @param word 期待する文字列
     * @return 一致したら true
     */
    bool consumeWord(const char* word) {
        size_t i = 0;
        while (word[i] != '\0') {
            if (pos_ + i >= src_.size() || src_[pos_ + i] != word[i]) return false;
            ++i;
        }
        pos_ += i;
        return true;
    }

    /**
     * 値をひとつ解析する。
     * @param out 結果の書き込み先
     * @param depth 現在の入れ子の深さ
     * @return 成功したら true
     */
    bool parseValue(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return fail("入れ子が深すぎます");
        skipSpace();
        if (pos_ >= src_.size()) return fail("値がありません");
        const char c = src_[pos_];
        if (c == '{') return parseObject(out, depth);
        if (c == '[') return parseArray(out, depth);
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return parseString(out.text);
        }
        if (consumeWord("true")) {
            out.type = JsonValue::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (consumeWord("false")) {
            out.type = JsonValue::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (consumeWord("null")) {
            out.type = JsonValue::Type::Null;
            return true;
        }
        return parseNumber(out);
    }

    /**
     * 数値を解析する。
     * @param out 結果の書き込み先
     * @return 成功したら true
     */
    bool parseNumber(JsonValue& out) {
        const size_t start = pos_;
        if (pos_ < src_.size() && (src_[pos_] == '-' || src_[pos_] == '+')) ++pos_;
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            const bool numberChar = (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+';
            if (!numberChar) break;
            ++pos_;
        }
        if (pos_ == start) return fail("解釈できない文字があります");
        const std::string token = src_.substr(start, pos_ - start);
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0') return fail("数値の形が正しくありません: " + token);
        out.type = JsonValue::Type::Number;
        out.number = value;
        return true;
    }

    /**
     * 4 桁の 16 進数を読む（\uXXXX 用）。
     * @param code 読んだ値の書き込み先
     * @return 成功したら true
     */
    bool parseHex4(unsigned& code) {
        if (pos_ + 4 > src_.size()) return fail("\\u の後ろが短すぎます");
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = src_[pos_++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') code |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("\\u の後ろが 16 進数ではありません");
        }
        return true;
    }

    /**
     * コードポイントを UTF-8 にして追加する。
     * @param code コードポイント
     * @param out 追加先
     */
    static void appendUtf8(unsigned code, std::string& out) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    /**
     * 文字列を解析する（先頭の " から）。
     * @param out 結果の書き込み先
     * @return 成功したら true
     */
    bool parseString(std::string& out) {
        ++pos_;  // 先頭の "
        out.clear();
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') {
                makeValidUtf8(out);  // 生のバイトの壊れた UTF-8 も U+FFFD に
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= src_.size()) break;
            const char e = src_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned code = 0;
                    if (!parseHex4(code)) return false;
                    // サロゲートペア。対になっていないサロゲートは U+FFFD にする（UTF-8 にできないので）。
                    // 後ろの \uXXXX が下位サロゲートでなければ読み戻し、普通の文字として次に読む
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        const size_t mark = pos_;
                        unsigned low = 0;
                        if (pos_ + 6 <= src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            if (!parseHex4(low)) return false;
                        }
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            pos_ = mark;
                            code = kReplacement;
                        }
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        code = kReplacement;
                    }
                    appendUtf8(code, out);
                    break;
                }
                default: return fail("文字列のエスケープが正しくありません");
            }
        }
        return fail("文字列が閉じていません");
    }

    /** 置き換え文字（U+FFFD）。壊れた文字はこれにする（1 文字のために設定ファイル全体を捨てない）。 */
    static constexpr unsigned kReplacement = 0xFFFD;

    /**
     * 配列を解析する（先頭の [ から）。
     * @param out 結果の書き込み先
     * @param depth 現在の入れ子の深さ
     * @return 成功したら true
     */
    bool parseArray(JsonValue& out, int depth) {
        ++pos_;  // [
        out.type = JsonValue::Type::Array;
        skipSpace();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue item;
            if (!parseValue(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            skipSpace();
            if (pos_ >= src_.size()) return fail("配列が閉じていません");
            const char c = src_[pos_++];
            if (c == ']') return true;
            if (c != ',') return fail("配列の区切りに , か ] が必要です");
        }
    }

    /**
     * オブジェクトを解析する（先頭の { から）。
     * @param out 結果の書き込み先
     * @param depth 現在の入れ子の深さ
     * @return 成功したら true
     */
    bool parseObject(JsonValue& out, int depth) {
        ++pos_;  // {
        out.type = JsonValue::Type::Object;
        skipSpace();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skipSpace();
            if (pos_ >= src_.size() || src_[pos_] != '"') return fail("キーは \"...\" で書いてください");
            std::string key;
            if (!parseString(key)) return false;
            skipSpace();
            if (pos_ >= src_.size() || src_[pos_] != ':') return fail("キーの後ろに : が必要です");
            ++pos_;
            JsonValue value;
            if (!parseValue(value, depth + 1)) return false;
            out.members.emplace_back(std::move(key), std::move(value));
            skipSpace();
            if (pos_ >= src_.size()) return fail("オブジェクトが閉じていません");
            const char c = src_[pos_++];
            if (c == '}') return true;
            if (c != ',') return fail("メンバーの区切りに , か } が必要です");
        }
    }
};

}  // namespace

const JsonValue* JsonValue::get(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& member : members) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

bool parseJson(const std::string& source, JsonValue& out, std::string& error) {
    out = JsonValue();
    Parser parser(source);
    return parser.parseDocument(out, error);
}

std::string jsonString(const std::string& text) {
    std::string out = "\"";
    out.reserve(text.size() + 2);
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : text) {
        if (c < 0x20) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            if (c == '"' || c == '\\') out += '\\';
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

namespace {

/**
 * text[i] から始まる 1 文字の UTF-8 の長さ。
 * @return 1〜4。壊れていれば 0
 */
size_t utf8Length(const unsigned char* p, size_t left) {
    const unsigned char c = p[0];
    if (c < 0x80) return 1;
    size_t n = 0;
    unsigned char lo = 0x80, hi = 0xBF;  // 2 バイト目の範囲（長すぎる形とサロゲートを除く）
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        if (c == 0xE0) lo = 0xA0;
        if (c == 0xED) hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        if (c == 0xF0) lo = 0x90;
        if (c == 0xF4) hi = 0x8F;
    } else {
        return 0;
    }
    if (left < n || p[1] < lo || p[1] > hi) return 0;
    for (size_t k = 2; k < n; ++k) {
        if (p[k] < 0x80 || p[k] > 0xBF) return 0;
    }
    return n;
}

}  // namespace

bool isValidUtf8(const char* text, size_t size) {
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    for (size_t i = 0; i < size;) {
        if (p[i] < 0x80) {  // ASCII はそのまま進む（ほとんどの文字列はここだけ）
            ++i;
            continue;
        }
        const size_t n = utf8Length(p + i, size - i);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

void makeValidUtf8(std::string& text) {
    if (isValidUtf8(text.data(), text.size())) return;
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    std::string out;
    out.reserve(text.size() + 8);
    for (size_t i = 0; i < text.size();) {
        const size_t n = utf8Length(p + i, text.size() - i);
        if (n == 0) {
            out += "\xEF\xBF\xBD";  // U+FFFD
            ++i;
        } else {
            out.append(text, i, n);
            i += n;
        }
    }
    text.swap(out);
}
