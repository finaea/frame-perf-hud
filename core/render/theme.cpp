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
        case ContrastKind::Text: return "text";
        case ContrastKind::Ui: return "UI";
        case ContrastKind::Disabled: return "disabled (guide only)";
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
        {"Perf panel: numbers (card)", kText, kCard, ContrastKind::Text},
        {"Perf panel: labels, units, notes (card)", kTextMuted, kCard, ContrastKind::Text},
        {"Perf panel: yellow numbers (card)", kWarn, kCard, ContrastKind::Text},
        {"Perf panel: red numbers (card)", kDanger, kCard, ContrastKind::Text},
        {"Perf panel: \"GPU\" label (card)", kGpu, kCard, ContrastKind::Text},
        {"Perf panel: graph tick labels (graph bg)", kTextMuted, kBg, ContrastKind::Text},
        {"Perf panel: throttled badge text (yellow fill)", kOnAccent, kWarn, ContrastKind::Text},
        {"Perf panel: thermal throttle badge text (red fill)", kOnAccent, kDanger, ContrastKind::Text},
        {"Perf panel: fps / power / CPU temp line (graph bg)", kAccent, kBg, ContrastKind::Ui},
        {"Perf panel: fps line (over GPU load fill)", kAccent, kGpuFill, ContrastKind::Ui},
        {"Perf panel: GPU temp line (graph bg)", kGpu, kBg, ContrastKind::Ui},
        {"Perf panel: yellow threshold line (graph bg)", kWarn, kBg, ContrastKind::Ui},
        {"Perf panel: red threshold line (graph bg)", kDanger, kBg, ContrastKind::Ui},
        {"Perf panel: target dotted line (graph bg)", kTextMuted, kBg, ContrastKind::Ui},
        {"Perf panel: legend ● (card)", kAccent, kCard, ContrastKind::Ui},
        {"Perf panel: GPU ● (card)", kGpu, kCard, ContrastKind::Ui},
        {"Perf panel: Wi-Fi icon (strong, card)", kText, kCard, ContrastKind::Ui},
        {"Perf panel: Wi-Fi icon (weak, card)", kWarn, kCard, ContrastKind::Ui},
        {"Perf panel: Wi-Fi icon (very weak, card)", kDanger, kCard, ContrastKind::Ui},
        {"Perf panel: Wi-Fi icon (disconnected, slash, card)", kTextMuted, kCard, ContrastKind::Ui},
        {"Perf panel: charging bolt (card)", kWarn, kCard, ContrastKind::Ui},
        {"Perf panel: thermal throttle border (panel bg)", kDanger, kBg, ContrastKind::Ui},
        // ---- 設定パネル ----
        {"Settings: heading (panel bg)", kText, kBg, ContrastKind::Text},
        {"Settings: body text, values (card)", kText, kCard, ContrastKind::Text},
        {"Settings: secondary text (panel bg)", kTextMuted, kBg, ContrastKind::Text},
        {"Settings: secondary text (card)", kTextMuted, kCard, ContrastKind::Text},
        {"Settings: button text", kText, kControl, ContrastKind::Text},
        {"Settings: button text (hovered or pressed)", kText, kControlHover, ContrastKind::Text},
        {"Settings: selected text (accent fill)", kOnAccent, kAccent, ContrastKind::Text},
        {"Settings: selected text (while pressed)", kOnAccent, kAccentPressed, ContrastKind::Text},
        {"Settings: disabled button text", kTextDisabled, kControl, ContrastKind::Disabled},
        {"Settings: \"panel shown\" pill", kSuccess, kSuccessTint, ContrastKind::Text},
        {"Settings: \"panel hidden\" pill", kTextMuted, kControl, ContrastKind::Text},
        {"Settings: switching text (panel bg)", kText, kBg, ContrastKind::Text},
        {"Settings: error text (panel bg)", kDanger, kBg, ContrastKind::Text},
        {"Settings: quit button text", kText, kQuitFill, ContrastKind::Text},
        {"Settings: quit button text (hovered)", kText, kControlHover, ContrastKind::Text},
        {"Settings: quit confirm text (red fill)", kOnAccent, kDanger, ContrastKind::Text},
        {"Settings: button / pill border (card)", kBorder, kCard, ContrastKind::Ui},
        {"Settings: button / pill border (panel bg)", kBorder, kBg, ContrastKind::Ui},
        {"Settings: selected fill (card)", kAccent, kCard, ContrastKind::Ui},
        {"Settings: selected fill (pill bg)", kAccent, kControl, ContrastKind::Ui},
        {"Settings: selected fill (panel bg)", kAccent, kBg, ContrastKind::Ui},
        {"Settings: pressed border (card)", kAccent, kCard, ContrastKind::Ui},
        {"Settings: \"panel shown\" ●", kSuccess, kSuccessTint, ContrastKind::Ui},
        {"Settings: \"panel hidden\" ○", kTextMuted, kControl, ContrastKind::Ui},
        {"Settings: quit button border (panel bg)", kDanger, kBg, ContrastKind::Ui},
        {"Settings: update banner \"new version available\" text (card)", kAccentText, kCard, ContrastKind::Text},
        {"Settings: update banner error text (card)", kDanger, kCard, ContrastKind::Text},
        {"Settings: update banner secondary text (card)", kTextMuted, kCard, ContrastKind::Text},
        {"Settings: update banner pink border (panel bg)", kAccent, kBg, ContrastKind::Ui},
        {"Settings: update banner pink border (card)", kAccent, kCard, ContrastKind::Ui},
        {"Settings: update banner red border (panel bg)", kDanger, kBg, ContrastKind::Ui},
        {"Settings: update banner red border (card)", kDanger, kCard, ContrastKind::Ui},
        {"Settings: update banner primary button text (accent fill)", kOnAccent, kAccent, ContrastKind::Text},
    };
    return pairs;
}

int printContrastReport() {
    int failures = 0;
    double lowest = 100.0;
    const ContrastPair* lowestPair = nullptr;
    std::printf("%-6s %-7s %-7s %6s %5s  %s\n", "result", "fg", "bg", "ratio", "need", "where it's used");
    for (const ContrastPair& pair : contrastPairs()) {
        const double ratio = contrastRatio(pair.fg, pair.bg);
        const double need = requiredRatio(pair.kind);
        const bool ok = ratio >= need;
        if (!ok) ++failures;
        if (ratio < lowest) {
            lowest = ratio;
            lowestPair = &pair;
        }
        std::printf("%-6s %s %s %6.2f %5.1f  %s (%s)\n", ok ? "PASS" : "FAIL", hexText(pair.fg).c_str(),
                    hexText(pair.bg).c_str(), ratio, need, pair.what, kindName(pair.kind));
    }
    if (lowestPair != nullptr) {
        std::printf("lowest ratio: %.2f (%s: %s / %s)\n", lowest, lowestPair->what, hexText(lowestPair->fg).c_str(),
                    hexText(lowestPair->bg).c_str());
    }
    std::printf("%zu pairs, %d failed\n", contrastPairs().size(), failures);
    return failures == 0 ? 0 : 1;
}
