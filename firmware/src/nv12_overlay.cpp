#include "nv12_overlay.h"

#include <rockchip/mpp_frame.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>

// 说明：本文件承载 NV12 帧的软件叠加渲染（点阵字库 + 画框/画字 + 检测框烧录）。
// 除对外的 draw_yolo_boxes_on_nv12 外，其余均为本翻译单元内部实现（匿名 namespace）。

namespace
{
struct Nv12Color
{
    uint8_t y = 0;
    uint8_t u = 128;
    uint8_t v = 128;
};

constexpr Nv12Color kColorBlack{16, 128, 128};
constexpr Nv12Color kColorWhite{235, 128, 128};
constexpr Nv12Color kColorYellow{210, 16, 146};

constexpr uint8_t kGlyphUnknown[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};  // '?'
constexpr uint8_t kGlyphSpace[7]   = {0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t kGlyphMinus[7]   = {0, 0, 0, 0x1F, 0, 0, 0};
constexpr uint8_t kGlyphDot[7]     = {0, 0, 0, 0, 0, 0x0C, 0x0C};
constexpr uint8_t kGlyphSlash[7]   = {0x01, 0x02, 0x04, 0x08, 0x10, 0, 0};
constexpr uint8_t kGlyphColon[7]   = {0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0};

constexpr uint8_t kGlyphDigits[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E},  // 5
    {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E},  // 9
};

constexpr uint8_t kGlyphUpper[26][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // A
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},  // B
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},  // C
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},  // D
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},  // E
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},  // F
    {0x0E, 0x11, 0x10, 0x10, 0x13, 0x11, 0x0E},  // G
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // H
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},  // I
    {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C},  // J
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},  // K
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},  // L
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},  // M
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},  // N
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // O
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},  // P
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},  // Q
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},  // R
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},  // S
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // T
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // U
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},  // V
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},  // W
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},  // X
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},  // Y
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},  // Z
};

const uint8_t* glyph_for_char(char c)
{
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc >= '0' && uc <= '9')
    {
        return kGlyphDigits[uc - '0'];
    }
    if (uc >= 'A' && uc <= 'Z')
    {
        return kGlyphUpper[uc - 'A'];
    }
    switch (uc)
    {
        case ' ':
            return kGlyphSpace;
        case '-':
            return kGlyphMinus;
        case '.':
            return kGlyphDot;
        case '/':
            return kGlyphSlash;
        case ':':
            return kGlyphColon;
        default:
            return kGlyphUnknown;
    }
}

void set_nv12_pixel(const IO_FD_t* frame, int x, int y, Nv12Color color)
{
    if (!frame || !frame->base)
    {
        return;
    }
    if (x < 0 || x >= static_cast<int>(frame->width) || y < 0 ||
        y >= static_cast<int>(frame->height))
    {
        return;
    }

    uint8_t* const y_plane = static_cast<uint8_t*>(frame->base);
    uint8_t* const uv_plane =
        y_plane + static_cast<size_t>(frame->hor_stride) * static_cast<size_t>(frame->ver_stride);
    const int y_stride  = static_cast<int>(frame->hor_stride);
    const int uv_stride = static_cast<int>(frame->hor_stride);

    y_plane[static_cast<size_t>(y) * static_cast<size_t>(y_stride) + static_cast<size_t>(x)] =
        color.y;
    const int uv_x = (x / 2) * 2;
    const int uv_y = y / 2;
    const size_t uv_offset =
        static_cast<size_t>(uv_y) * static_cast<size_t>(uv_stride) + static_cast<size_t>(uv_x);
    uv_plane[uv_offset]     = color.u;
    uv_plane[uv_offset + 1] = color.v;
}

void draw_filled_rect_nv12(const IO_FD_t* frame, int left, int top, int right, int bottom,
                           Nv12Color color)
{
    if (!frame)
    {
        return;
    }
    const int width  = static_cast<int>(frame->width);
    const int height = static_cast<int>(frame->height);
    left             = std::clamp(left, 0, width - 1);
    right            = std::clamp(right, 0, width - 1);
    top              = std::clamp(top, 0, height - 1);
    bottom           = std::clamp(bottom, 0, height - 1);
    if (right < left || bottom < top)
    {
        return;
    }
    for (int y = top; y <= bottom; ++y)
    {
        for (int x = left; x <= right; ++x)
        {
            set_nv12_pixel(frame, x, y, color);
        }
    }
}

void draw_rect_nv12(const IO_FD_t* frame, int left, int top, int right, int bottom, int thickness,
                    Nv12Color color)
{
    if (!frame || thickness <= 0)
    {
        return;
    }
    for (int t = 0; t < thickness; ++t)
    {
        const int l = left + t;
        const int r = right - t;
        const int u = top + t;
        const int d = bottom - t;
        if (l > r || u > d)
        {
            break;
        }
        for (int x = l; x <= r; ++x)
        {
            set_nv12_pixel(frame, x, u, color);
            set_nv12_pixel(frame, x, d, color);
        }
        for (int y = u; y <= d; ++y)
        {
            set_nv12_pixel(frame, l, y, color);
            set_nv12_pixel(frame, r, y, color);
        }
    }
}

void draw_char_nv12(const IO_FD_t* frame, int x, int y, char c, int scale, Nv12Color color)
{
    if (!frame || scale <= 0)
    {
        return;
    }
    const char      up    = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const uint8_t*  glyph = glyph_for_char(up);
    constexpr int   gw    = 5;
    constexpr int   gh    = 7;
    for (int row = 0; row < gh; ++row)
    {
        const uint8_t bits = glyph[row];
        for (int col = 0; col < gw; ++col)
        {
            if (((bits >> (gw - 1 - col)) & 0x01u) == 0u)
            {
                continue;
            }
            const int px = x + col * scale;
            const int py = y + row * scale;
            draw_filled_rect_nv12(frame, px, py, px + scale - 1, py + scale - 1, color);
        }
    }
}

void draw_text_nv12(const IO_FD_t* frame, int x, int y, const std::string& text, int scale,
                    Nv12Color fg, Nv12Color bg)
{
    if (!frame || text.empty())
    {
        return;
    }
    constexpr int glyph_w    = 5;
    constexpr int glyph_h    = 7;
    constexpr int char_space = 1;
    const int     char_step  = (glyph_w + char_space) * scale;
    const int     text_w     = static_cast<int>(text.size()) * char_step;
    const int     text_h     = glyph_h * scale;
    const int     pad        = 3;
    draw_filled_rect_nv12(frame, x - pad, y - pad, x + text_w + pad, y + text_h + pad, bg);
    int cursor_x = x;
    for (char c : text)
    {
        draw_char_nv12(frame, cursor_x, y, c, scale, fg);
        cursor_x += char_step;
    }
}
}  // namespace

void draw_yolo_boxes_on_nv12(const IO_FD_t* frame,
                             const std::vector<YoloNpuInstance::DetectBox>& boxes,
                             const std::function<std::string(int)>& label_of)
{
    if (!frame || !frame->base || frame->width == 0 || frame->height == 0 || frame->hor_stride == 0 ||
        frame->ver_stride == 0)
    {
        return;
    }
    if ((frame->format & MPP_FRAME_FMT_MASK) != MPP_FMT_YUV420SP)
    {
        return;
    }

    const int    width    = static_cast<int>(frame->width);
    const int    height   = static_cast<int>(frame->height);
    const size_t max_box  = std::min<size_t>(boxes.size(), 20u);
    const int    outer_th = 5;
    const int    inner_th = 3;

    for (size_t i = 0; i < max_box; ++i)
    {
        const auto& box = boxes[i];
        int         left   = std::clamp(box.left, 0, width - 1);
        int         top    = std::clamp(box.top, 0, height - 1);
        int         right  = std::clamp(box.right, 0, width - 1);
        int         bottom = std::clamp(box.bottom, 0, height - 1);
        if (right <= left || bottom <= top)
        {
            continue;
        }

        draw_rect_nv12(frame, left, top, right, bottom, outer_th, kColorBlack);
        draw_rect_nv12(frame, left + 1, top + 1, right - 1, bottom - 1, inner_th, kColorYellow);

        char score_buf[16] = {0};
        (void)std::snprintf(score_buf, sizeof(score_buf), "%.2f", box.score);
        std::string label = label_of ? label_of(box.cls_id) : std::string();
        if (label.empty() || label == "unknown")
        {
            label = "cls_" + std::to_string(box.cls_id);
        }
        std::string text = label + " " + score_buf;
        if (text.size() > 36u)
        {
            text.resize(36u);
        }

        const int text_scale = 2;
        const int text_w     = static_cast<int>(text.size()) * (5 + 1) * text_scale;
        const int text_h     = 7 * text_scale;
        int       text_x     = left + 2;
        int       text_y     = top - (text_h + 8);
        if (text_y < 0)
        {
            text_y = top + 4;
        }
        if (text_x + text_w >= width)
        {
            text_x = std::max(0, width - text_w - 2);
        }
        draw_text_nv12(frame, text_x, text_y, text, text_scale, kColorWhite, kColorBlack);
    }
}
