// 色の定義とコントラスト比の計算の実装。
#include "theme.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

/**
 * sRGB の 1 チャンネルを線形の値にする（WCAG 2.x の式）。
 * @param c 0〜1
 * @return 線形の値
 */
double linearChannel(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

/**
 * 色を #rrggbb にする（表示用）。
 * @param c 色
 * @return 文字列
 */
std::string hexText(Color c) {
    char text[16];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", static_cast<int>(std::lround(c.r * 255)),
                  static_cast<int>(std::lround(c.g * 255)), static_cast<int>(std::lround(c.b * 255)));
    return text;
}

/**
 * 種類の名前（表示用）。
 * @param kind 種類
 * @return 名前
 */
const char* kindName(ContrastKind kind) {
    switch (kind) {
        case ContrastKind::Text: return "文字";
        case ContrastKind::Ui: return "部品";
        case ContrastKind::Disabled: return "無効（目安）";
    }
    return "";
}

}  // namespace

double relativeLuminance(Color c) {
    return 0.2126 * linearChannel(c.r) + 0.7152 * linearChannel(c.g) + 0.0722 * linearChannel(c.b);
}

double contrastRatio(Color a, Color b) {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    const double light = la > lb ? la : lb;
    const double dark = la > lb ? lb : la;
    return (light + 0.05) / (dark + 0.05);
}

double requiredRatio(ContrastKind kind) {
    return kind == ContrastKind::Text ? 4.5 : 3.0;
}

const std::vector<ContrastPair>& contrastPairs() {
    // 描画で使っている組み合わせをすべて並べる（文字は大きさによらず、すべて 4.5:1 で確かめる）
    static const std::vector<ContrastPair> pairs = {
        // ---- 性能パネル（数字と見出しは段のカードの上） ----
        {"性能パネル: 数字（カード）", kText, kCard, ContrastKind::Text},
        {"性能パネル: 見出し・単位・補足（カード）", kTextMuted, kCard, ContrastKind::Text},
        {"性能パネル: 黄の数字（カード）", kWarn, kCard, ContrastKind::Text},
        {"性能パネル: 赤の数字（カード）", kDanger, kCard, ContrastKind::Text},
        {"性能パネル: 「GPU」の文字（カード）", kGpu, kCard, ContrastKind::Text},
        {"性能パネル: グラフの目盛りの文字（グラフの台）", kTextMuted, kBg, ContrastKind::Text},
        {"性能パネル: 制限中のバッジの文字（黄の塗り）", kOnAccent, kWarn, ContrastKind::Text},
        {"性能パネル: 熱で制限中のバッジの文字（赤の塗り）", kOnAccent, kDanger, ContrastKind::Text},
        {"性能パネル: fps・電力・CPU 温度の線（グラフの台）", kAccent, kBg, ContrastKind::Ui},
        {"性能パネル: fps の線（GPU 負荷の塗りの上）", kAccent, kGpuFill, ContrastKind::Ui},
        {"性能パネル: GPU 温度の線（グラフの台）", kGpu, kBg, ContrastKind::Ui},
        {"性能パネル: 黄のしきい値の線（グラフの台）", kWarn, kBg, ContrastKind::Ui},
        {"性能パネル: 赤のしきい値の線（グラフの台）", kDanger, kBg, ContrastKind::Ui},
        {"性能パネル: 目標の点線（グラフの台）", kTextMuted, kBg, ContrastKind::Ui},
        {"性能パネル: 凡例の ●（カード）", kAccent, kCard, ContrastKind::Ui},
        {"性能パネル: GPU の ●（カード）", kGpu, kCard, ContrastKind::Ui},
        {"性能パネル: Wi-Fi アイコン（強・カード）", kText, kCard, ContrastKind::Ui},
        {"性能パネル: Wi-Fi アイコン（弱・カード）", kWarn, kCard, ContrastKind::Ui},
        {"性能パネル: Wi-Fi アイコン（とても弱い・カード）", kDanger, kCard, ContrastKind::Ui},
        {"性能パネル: Wi-Fi アイコン（未接続・斜線、カード）", kTextMuted, kCard, ContrastKind::Ui},
        {"性能パネル: 充電中の稲妻（カード）", kWarn, kCard, ContrastKind::Ui},
        {"性能パネル: 熱で制限中の枠（パネルの地）", kDanger, kBg, ContrastKind::Ui},
        // ---- 設定パネル ----
        {"設定: 見出し（パネルの地）", kText, kBg, ContrastKind::Text},
        {"設定: 本文・値（カード）", kText, kCard, ContrastKind::Text},
        {"設定: 補足の文字（パネルの地）", kTextMuted, kBg, ContrastKind::Text},
        {"設定: 補足の文字（カード）", kTextMuted, kCard, ContrastKind::Text},
        {"設定: ボタンの文字", kText, kControl, ContrastKind::Text},
        {"設定: ボタンの文字（ポインターが乗っている・押している）", kText, kControlHover, ContrastKind::Text},
        {"設定: 選択中の文字（アクセントの塗り）", kOnAccent, kAccent, ContrastKind::Text},
        {"設定: 選択中の文字（押している間）", kOnAccent, kAccentPressed, ContrastKind::Text},
        {"設定: 押せないボタンの文字", kTextDisabled, kControl, ContrastKind::Disabled},
        {"設定: 「パネル表示中」のピル", kSuccess, kSuccessTint, ContrastKind::Text},
        {"設定: 「パネル非表示」のピル", kTextMuted, kControl, ContrastKind::Text},
        {"設定: 切り替え中の文字（パネルの地）", kText, kBg, ContrastKind::Text},
        {"設定: 失敗の文字（パネルの地）", kDanger, kBg, ContrastKind::Text},
        {"設定: 終了ボタンの文字", kText, kQuitFill, ContrastKind::Text},
        {"設定: 終了ボタンの文字（ポインターが乗っている）", kText, kControlHover, ContrastKind::Text},
        {"設定: 終了の確認中の文字（赤い塗り）", kOnAccent, kDanger, ContrastKind::Text},
        {"設定: ボタン・ピルの枠（カード）", kBorder, kCard, ContrastKind::Ui},
        {"設定: ボタン・ピルの枠（パネルの地）", kBorder, kBg, ContrastKind::Ui},
        {"設定: 選択中の塗り（カード）", kAccent, kCard, ContrastKind::Ui},
        {"設定: 選択中の塗り（ピルの地）", kAccent, kControl, ContrastKind::Ui},
        {"設定: 選択中の塗り（パネルの地）", kAccent, kBg, ContrastKind::Ui},
        {"設定: 押している間の枠（カード）", kAccent, kCard, ContrastKind::Ui},
        {"設定: 「パネル表示中」の ●", kSuccess, kSuccessTint, ContrastKind::Ui},
        {"設定: 「パネル非表示」の ○", kTextMuted, kControl, ContrastKind::Ui},
        {"設定: 終了ボタンの枠（パネルの地）", kDanger, kBg, ContrastKind::Ui},
        {"設定: 更新の帯の「新しい版があります」の文字（カード）", kAccentText, kCard, ContrastKind::Text},
        {"設定: 更新の帯の失敗の文字（カード）", kDanger, kCard, ContrastKind::Text},
        {"設定: 更新の帯の補足の文字（カード）", kTextMuted, kCard, ContrastKind::Text},
        {"設定: 更新の帯のピンクの枠（パネルの地）", kAccent, kBg, ContrastKind::Ui},
        {"設定: 更新の帯のピンクの枠（カード）", kAccent, kCard, ContrastKind::Ui},
        {"設定: 更新の帯の赤い枠（パネルの地）", kDanger, kBg, ContrastKind::Ui},
        {"設定: 更新の帯の赤い枠（カード）", kDanger, kCard, ContrastKind::Ui},
        {"設定: 更新の帯の強調ボタンの文字（アクセントの塗り）", kOnAccent, kAccent, ContrastKind::Text},
    };
    return pairs;
}

int printContrastReport() {
    int failures = 0;
    double lowest = 100.0;
    const ContrastPair* lowestPair = nullptr;
    std::printf("%-6s %-7s %-7s %6s %5s  %s\n", "結果", "文字色", "背景", "比", "必要", "どこで使っているか");
    for (const ContrastPair& pair : contrastPairs()) {
        const double ratio = contrastRatio(pair.fg, pair.bg);
        const double need = requiredRatio(pair.kind);
        const bool ok = ratio >= need;
        if (!ok) ++failures;
        if (ratio < lowest) {
            lowest = ratio;
            lowestPair = &pair;
        }
        std::printf("%-6s %s %s %6.2f %5.1f  %s（%s）\n", ok ? "合格" : "不合格", hexText(pair.fg).c_str(),
                    hexText(pair.bg).c_str(), ratio, need, pair.what, kindName(pair.kind));
    }
    if (lowestPair != nullptr) {
        std::printf("いちばん低い比: %.2f（%s: %s / %s）\n", lowest, lowestPair->what, hexText(lowestPair->fg).c_str(),
                    hexText(lowestPair->bg).c_str());
    }
    std::printf("%zu 組中 %d 組が不合格\n", contrastPairs().size(), failures);
    return failures == 0 ? 0 : 1;
}
