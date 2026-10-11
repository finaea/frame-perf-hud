// 設定ファイル用の小さな JSON パーサ（読み込み専用）。
#pragma once

#include <string>
#include <utility>
#include <vector>

/**
 * JSON の値ひとつ。オブジェクトはキーの順番を保ったままペアの配列で持つ。
 */
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<JsonValue> items;                              ///< Array の要素
    std::vector<std::pair<std::string, JsonValue>> members;    ///< Object のメンバー

    /**
     * オブジェクトからキーで値を探す。
     * @param key 探すキー
     * @return 見つかった値へのポインタ。オブジェクトでない・キーが無いときは nullptr
     */
    const JsonValue* get(const std::string& key) const;

    /** @return 数値なら true */
    bool isNumber() const { return type == Type::Number; }
    /** @return 真偽値なら true */
    bool isBool() const { return type == Type::Bool; }
    /** @return 文字列なら true */
    bool isString() const { return type == Type::String; }
    /** @return オブジェクトなら true */
    bool isObject() const { return type == Type::Object; }
};

/**
 * JSON 文字列を解析する。
 * @param source 解析する文字列（UTF-8）
 * @param out 解析結果の書き込み先
 * @param error 失敗したときの理由（何文字目か付き）
 * @return 成功したら true
 */
bool parseJson(const std::string& source, JsonValue& out, std::string& error);
