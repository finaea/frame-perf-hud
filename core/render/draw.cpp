// 描画の共通部品の実装。
#include "draw.h"

#include "json.h"   // isValidUtf8 / makeValidUtf8
#include "theme.h"  // the segmented pill's colours

#include <cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

/**
 * cairo の決まりに沿って、フォントが要らなくなったら FT_Face を閉じるための後始末。
 * @param face FT_Face
 */
void destroyFtFace(void* face) {
    FT_Done_Face(static_cast<FT_Face>(face));
}

const cairo_user_data_key_t kFtFaceKey {};

}  // namespace

FontSet::FontSet() {
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) == 0) ftLibrary_ = library;
}

FontSet::~FontSet() {
    release();
    // FT_Face は cairo が持っている間は生きている必要があるので、FT_Library は閉じない（終了時に OS が片付ける）
}

cairo_font_face_t* FontSet::createFace(const std::string& path, bool bold) {
    FT_Face face = nullptr;
    if (ftLibrary_ != nullptr && FT_New_Face(static_cast<FT_Library>(ftLibrary_), path.c_str(), 0, &face) == 0) {
        cairo_font_face_t* cairoFace = cairo_ft_font_face_create_for_ft_face(face, 0);
        if (cairo_font_face_set_user_data(cairoFace, &kFtFaceKey, face, destroyFtFace) == CAIRO_STATUS_SUCCESS) {
            return cairoFace;
        }
        cairo_font_face_destroy(cairoFace);
        FT_Done_Face(face);
    }
    std::fprintf(stderr, "[描画] フォント %s を読めないので Noto Sans CJK JP を探して使います\n", path.c_str());
    return cairo_toy_font_face_create("Noto Sans CJK JP", CAIRO_FONT_SLANT_NORMAL,
                                      bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
}

void FontSet::release() {
    // cairo_t が参照を持っていれば、そちらが手放すまでフォントは生きている
    if (regular_ != nullptr) cairo_font_face_destroy(regular_);
    if (bold_ != nullptr) cairo_font_face_destroy(bold_);
    regular_ = nullptr;
    bold_ = nullptr;
}

void FontSet::load(const std::string& regularPath, const std::string& boldPath) {
    if (regular_ != nullptr && regularPath == regularPath_ && boldPath == boldPath_) return;
    release();
    regular_ = createFace(regularPath, false);
    bold_ = createFace(boldPath, true);
    regularPath_ = regularPath;
    boldPath_ = boldPath;
}

namespace {

/**
 * 壊れた UTF-8 を描かない: cairo は一度 CAIRO_STATUS_INVALID_STRING になるとその cairo_t で二度と描けないので、
 * 渡す前に U+FFFD に置き換える。正しい文字列（ほとんど全部）は確かめるだけで、そのまま返す。
 * @param text 文字列
 * @param storage 置き換えたときの置き場所
 * @return 正しい UTF-8 の文字列
 */
const char* validUtf8(const char* text, std::string& storage) {
    if (isValidUtf8(text, std::strlen(text))) return text;
    storage = text;
    makeValidUtf8(storage);
    return storage.c_str();
}

}  // namespace

double Pen::measure(const char* text, double size, bool isBold) const {
    std::string fixed;
    text = validUtf8(text, fixed);
    cairo_set_font_face(cr, isBold ? fonts->bold() : fonts->regular());
    cairo_set_font_size(cr, size);
    cairo_text_extents_t extents;
    cairo_text_extents(cr, text, &extents);
    return extents.x_advance;
}

double Pen::text(double x, double y, const char* text, double size, Color c, bool isBold,
                 bool alignRight) const {
    std::string fixed;
    text = validUtf8(text, fixed);
    color(c);
    if (alignRight) {
        const double width = measure(text, size, isBold);
        cairo_move_to(cr, x - width, y);
        cairo_show_text(cr, text);
        return width;
    }
    // 左揃えは先に幅を測らず、描いたあとの現在位置から幅を出す（文字の組み立てを 1 回で済ませる）
    cairo_set_font_face(cr, isBold ? fonts->bold() : fonts->regular());
    cairo_set_font_size(cr, size);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
    double endX = x;
    double endY = y;
    cairo_get_current_point(cr, &endX, &endY);
    return endX - x;
}

void Pen::roundedRect(double x, double y, double w, double h, double r) const {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void Pen::dot(double x, double y, Color c) const {
    color(c);
    cairo_arc(cr, x, y, 4.0, 0, 2 * M_PI);
    cairo_fill(cr);
}

double fitSize(const Pen& pen, const char* text, double size, double minSize, double maxWidth, bool bold) {
    while (size > minSize && pen.measure(text, size, bold) > maxWidth) size -= 1;
    return size;
}

double centerBaseline(double top, double h, double size) {
    return top + h / 2 + size * 0.36;
}

void textCentered(const Pen& pen, double cx, double baseline, const char* text, double size, Color c, bool bold) {
    pen.text(cx - pen.measure(text, size, bold) / 2, baseline, text, size, c, bold);
}

void drawCard(const Pen& pen, double x, double y, double w, double h, double r, Color fill, Color border,
              double borderWidth) {
    cairo_t* cr = pen.cr;
    // 影（下にずらした黒を 2 段重ねる）
    cairo_set_source_rgba(cr, 0, 0, 0, 0.22);
    pen.roundedRect(x - 2, y + 5, w + 4, h + 4, r + 2);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.30);
    pen.roundedRect(x, y + 2, w, h + 1, r);
    cairo_fill(cr);
    pen.color(fill);
    pen.roundedRect(x, y, w, h, r);
    cairo_fill(cr);
    if (borderWidth > 0) strokeRounded(pen, x, y, w, h, r, border, borderWidth);
    // 内側のハイライト（上の縁だけ少し明るく）
    cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
    cairo_set_line_width(cr, 1);
    const double inset = borderWidth + 0.5;
    cairo_move_to(cr, x + r, y + inset);
    cairo_line_to(cr, x + w - r, y + inset);
    cairo_stroke(cr);
}

void strokeRounded(const Pen& pen, double x, double y, double w, double h, double r, Color c, double width) {
    pen.color(c);
    cairo_set_line_width(pen.cr, width);
    pen.roundedRect(x + width / 2, y + width / 2, w - width, h - width, std::max(0.0, r - width / 2));
    cairo_stroke(pen.cr);
}

void drawCheck(cairo_t* cr, double cx, double cy, double s, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_set_line_width(cr, s * 0.16);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, cx - s * 0.36, cy + s * 0.02);
    cairo_line_to(cr, cx - s * 0.10, cy + s * 0.28);
    cairo_line_to(cr, cx + s * 0.38, cy - s * 0.26);
    cairo_stroke(cr);
}

void drawDisc(cairo_t* cr, double cx, double cy, double r, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_fill(cr);
}

void drawSegmentedPill(const Pen& pen, double x, double y, double w, double h, const std::vector<PillPart>& parts,
                       int selected, bool usable, double size) {
    if (parts.empty()) return;
    cairo_t* cr = pen.cr;
    const double r = h / 2;
    // 地のピル。押せるときは枠（3:1 以上）で部品の形を見せる。押せないときは枠なし
    pen.color(kControl);
    pen.roundedRect(x, y, w, h, r);
    cairo_fill(cr);
    if (usable) strokeRounded(pen, x, y, w, h, r, kBorder, 2);

    const double inset = 5;
    const double part = (w - inset * 2) / static_cast<double>(parts.size());
    for (size_t i = 0; i < parts.size(); ++i) {
        const double sx = x + inset + part * static_cast<double>(i);
        const double sy = y + inset;
        const double sh = h - inset * 2;
        const bool pressed = usable && parts[i].pressed;
        const bool hovered = usable && parts[i].hovered;
        const bool isSelected = selected == static_cast<int>(i);
        if (isSelected) {
            // 選択中の部分: アクセントの塗り（押している間は少し濃く）＋ ✓ ＋ 太字。押せないときは薄く残す
            pen.color(pressed ? kAccentPressed : kAccent, usable ? 1.0 : 0.35);
            pen.roundedRect(sx, sy, part, sh, sh / 2);
            cairo_fill(cr);
        } else if (pressed || hovered) {
            pen.color(kControlHover);
            pen.roundedRect(sx, sy, part, sh, sh / 2);
            cairo_fill(cr);
        }
        const std::string& label = parts[i].label;
        const Color textColor = !usable ? kTextDisabled : (isSelected ? kOnAccent : kText);
        const double checkW = isSelected ? size * 0.9 : 0;
        const double labelSize = fitSize(pen, label, size, size * 0.7, part - 20 - checkW, isSelected);
        const double textW = pen.measure(label, labelSize, isSelected) + checkW;
        const double tx = sx + (part - textW) / 2;
        if (isSelected) drawCheck(cr, tx + checkW * 0.4, sy + sh / 2, size * 0.72, usable ? kOnAccent : kTextDisabled);
        pen.text(tx + checkW, centerBaseline(sy, sh, labelSize), label, labelSize, textColor, isSelected);
    }
}

void surfaceToRgba(cairo_surface_t* surface, std::vector<uint8_t>& out) {
    // 乗算済みを戻す計算（c * 255 / a）を前もって表にしておく（割り算を画素ごとにしない）。
    // 表は初回にメモリ上で作るので、実行ファイルには 64 KiB の表を持たない
    static const auto table = [] {
        std::vector<uint8_t> values(256 * 256);
        for (unsigned a = 1; a < 256; ++a)
            for (unsigned c = 0; c < 256; ++c)
                values[a * 256 + c] = static_cast<uint8_t>(std::min(255u, (c * 255 + a / 2) / a));
        return values;
    }();

    cairo_surface_flush(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const uint8_t* data = cairo_image_surface_get_data(surface);
    out.resize(static_cast<size_t>(width) * height * 4);
    uint8_t* dst = out.data();
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(data + static_cast<size_t>(y) * stride);
        int x = 0;
        // cairo は乗算済みの ARGB（ネイティブエンディアンの 32bit）なので、戻して RGBA に並べ替える。
        // 透明度は 4 画素まとめて見る（全部不透明・全部透明なら表を引かない。半透明の画像でも遅くしない）
        for (; x <= width - 4; x += 4) {
            const uint32_t p0 = row[x], p1 = row[x + 1], p2 = row[x + 2], p3 = row[x + 3];
            if (((p0 & p1 & p2 & p3) >> 24) == 255) {
                dst[0] = static_cast<uint8_t>(p0 >> 16);
                dst[1] = static_cast<uint8_t>(p0 >> 8);
                dst[2] = static_cast<uint8_t>(p0);
                dst[3] = 255;
                dst[4] = static_cast<uint8_t>(p1 >> 16);
                dst[5] = static_cast<uint8_t>(p1 >> 8);
                dst[6] = static_cast<uint8_t>(p1);
                dst[7] = 255;
                dst[8] = static_cast<uint8_t>(p2 >> 16);
                dst[9] = static_cast<uint8_t>(p2 >> 8);
                dst[10] = static_cast<uint8_t>(p2);
                dst[11] = 255;
                dst[12] = static_cast<uint8_t>(p3 >> 16);
                dst[13] = static_cast<uint8_t>(p3 >> 8);
                dst[14] = static_cast<uint8_t>(p3);
                dst[15] = 255;
            } else if (((p0 | p1 | p2 | p3) >> 24) == 0) {
                std::fill_n(dst, 16, uint8_t{0});
            } else {
                const uint8_t* line0 = table.data() + (p0 >> 24) * 256;
                dst[0] = line0[(p0 >> 16) & 255];
                dst[1] = line0[(p0 >> 8) & 255];
                dst[2] = line0[p0 & 255];
                dst[3] = static_cast<uint8_t>(p0 >> 24);
                const uint8_t* line1 = table.data() + (p1 >> 24) * 256;
                dst[4] = line1[(p1 >> 16) & 255];
                dst[5] = line1[(p1 >> 8) & 255];
                dst[6] = line1[p1 & 255];
                dst[7] = static_cast<uint8_t>(p1 >> 24);
                const uint8_t* line2 = table.data() + (p2 >> 24) * 256;
                dst[8] = line2[(p2 >> 16) & 255];
                dst[9] = line2[(p2 >> 8) & 255];
                dst[10] = line2[p2 & 255];
                dst[11] = static_cast<uint8_t>(p2 >> 24);
                const uint8_t* line3 = table.data() + (p3 >> 24) * 256;
                dst[12] = line3[(p3 >> 16) & 255];
                dst[13] = line3[(p3 >> 8) & 255];
                dst[14] = line3[p3 & 255];
                dst[15] = static_cast<uint8_t>(p3 >> 24);
            }
            dst += 16;
        }
        for (; x < width; ++x) {
            const uint32_t p = row[x], a = p >> 24;
            const uint8_t* line = table.data() + a * 256;
            dst[0] = line[(p >> 16) & 255];
            dst[1] = line[(p >> 8) & 255];
            dst[2] = line[p & 255];
            dst[3] = static_cast<uint8_t>(a);
            dst += 4;
        }
    }
}
