#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "c2d_helper.h"
#include "colours.h"
#include "config.h"
#include "fs.h"
#include "gui.h"
#include "osk.h"
#include "touch.h"
#include "utils.h"

namespace GUI {
    namespace {
        static constexpr int draw_canvas_x = 8;
        static constexpr int draw_canvas_y = 48;
        static constexpr int draw_canvas_w = 304;
        static constexpr int draw_canvas_h = 144;

        static constexpr int draw_button_y1 = 24;
        static constexpr int draw_button_y2 = 44;

        static constexpr int draw_message_line_chars = 48;

        enum DrawTool {
            DRAW_TOOL_PEN = 0,
            DRAW_TOOL_ERASER = 1
        };

        struct DrawButton {
            int x1;
            int x2;
            const char *label;
        };

        static const DrawButton draw_buttons[] = {
            { 8, 60, "SAVE" },
            { 68, 120, "PEN" },
            { 128, 180, "ERASE" },
            { 188, 240, "CLEAR" },
            { 248, 312, "BACK" }
        };

        static bool draw_initialized = false;
        static bool draw_dirty_texture = true;
        static bool draw_has_unsaved = false;
        static bool draw_exit_armed = false;
        static bool draw_prev_valid = false;
        static u64 draw_exit_armed_until = 0;
        static u64 draw_message_expire_at = 0;

        static DrawTool draw_tool = DRAW_TOOL_PEN;
        static u16 draw_prev_x = 0;
        static u16 draw_prev_y = 0;

        static std::string draw_message = "Stylus draw | A save | X tool | Y clear | B back";
        static std::string draw_default_name = "drawing.pbm";
        static std::vector<u8> draw_pixels;

        static C2D_Image draw_texture = { 0 };
        static bool draw_texture_ready = false;

        static u32 draw_next_pow2(u32 value) {
            u32 out = 1;

            while (out < value)
                out <<= 1;

            return out;
        }

        static void draw_set_message(const std::string &message, u32 ttl_ms = 0) {
            draw_message = message;
            draw_message_expire_at = (ttl_ms == 0) ? 0 : (osGetTime() + ttl_ms);
        }

        static void draw_reset_message_if_expired(void) {
            if (draw_message_expire_at == 0)
                return;

            if (osGetTime() < draw_message_expire_at)
                return;

            draw_message_expire_at = 0;
            draw_set_message("Stylus draw | A save | X tool | Y clear | B back");
        }

        static bool draw_init_texture(void) {
            u32 tex_w;
            u32 tex_h;
            C3D_Tex *tex;
            Tex3DS_SubTexture *subtex;

            if (draw_texture_ready)
                return true;

            tex_w = draw_next_pow2(static_cast<u32>(draw_canvas_w));
            tex_h = draw_next_pow2(static_cast<u32>(draw_canvas_h));

            tex = new C3D_Tex();
            if (tex == nullptr)
                return false;

            if (!C3D_TexInit(tex, static_cast<u16>(tex_w), static_cast<u16>(tex_h), GPU_RGBA8)) {
                delete tex;
                return false;
            }

            C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

            subtex = new Tex3DS_SubTexture();
            if (subtex == nullptr) {
                C3D_TexDelete(tex);
                delete tex;
                return false;
            }

            subtex->width = static_cast<u16>(draw_canvas_w);
            subtex->height = static_cast<u16>(draw_canvas_h);
            subtex->left = 0.0f;
            subtex->top = 1.0f;
            subtex->right = draw_canvas_w / static_cast<float>(tex_w);
            subtex->bottom = 1.0f - (draw_canvas_h / static_cast<float>(tex_h));

            draw_texture.tex = tex;
            draw_texture.subtex = subtex;
            draw_texture_ready = true;
            draw_dirty_texture = true;
            return true;
        }

        static bool draw_update_texture(void) {
            u8 *texture_data;
            u32 texture_w;
            int x;
            int y;

            if (!draw_dirty_texture)
                return true;

            if (!draw_init_texture())
                return false;

            texture_data = static_cast<u8 *>(draw_texture.tex->data);
            texture_w = static_cast<u32>(draw_texture.tex->width);

            std::memset(texture_data, 0xFF, draw_texture.tex->size);

            for (y = 0; y < draw_canvas_h; y++) {
                for (x = 0; x < draw_canvas_w; x++) {
                    const size_t index = static_cast<size_t>(y * draw_canvas_w + x);

                    if (draw_pixels[index] == 0)
                        continue;

                    const u32 dst_pos = ((((static_cast<u32>(y) >> 3) * (texture_w >> 3) + (static_cast<u32>(x) >> 3)) << 6)
                        + ((static_cast<u32>(x) & 1)
                        | ((static_cast<u32>(y) & 1) << 1)
                        | ((static_cast<u32>(x) & 2) << 1)
                        | ((static_cast<u32>(y) & 2) << 2)
                        | ((static_cast<u32>(x) & 4) << 2)
                        | ((static_cast<u32>(y) & 4) << 3))) * 4;

                    texture_data[dst_pos + 0] = 0xFF;
                    texture_data[dst_pos + 1] = 0x00;
                    texture_data[dst_pos + 2] = 0x00;
                    texture_data[dst_pos + 3] = 0x00;
                }
            }

            C3D_TexFlush(draw_texture.tex);
            draw_dirty_texture = false;
            return true;
        }

        static void draw_ensure_initialized(void) {
            if (draw_initialized)
                return;

            draw_pixels.assign(static_cast<size_t>(draw_canvas_w * draw_canvas_h), 0);
            draw_initialized = true;
            draw_dirty_texture = true;
            draw_set_message("Stylus draw | A save | X tool | Y clear | B back");
        }

        static void draw_reset_canvas(bool mark_unsaved, const std::string &message, u32 ttl_ms = 0) {
            draw_ensure_initialized();
            std::fill(draw_pixels.begin(), draw_pixels.end(), 0);
            draw_dirty_texture = true;
            draw_has_unsaved = mark_unsaved;
            draw_exit_armed = false;
            draw_prev_valid = false;
            draw_set_message(message, ttl_ms);
        }

        static bool draw_skip_pbm_header_space(const std::string &content, size_t *position) {
            if (!position)
                return false;

            while (*position < content.size()) {
                const unsigned char current = static_cast<unsigned char>(content[*position]);

                if (current == '#') {
                    while (*position < content.size() && content[*position] != '\n')
                        (*position)++;

                    continue;
                }

                if (!std::isspace(current))
                    return true;

                (*position)++;
            }

            return false;
        }

        static bool draw_read_pbm_token(const std::string &content, size_t *position, std::string *out_token) {
            size_t start = 0;

            if (!position || !out_token)
                return false;

            if (!draw_skip_pbm_header_space(content, position))
                return false;

            start = *position;
            while (*position < content.size()) {
                const unsigned char current = static_cast<unsigned char>(content[*position]);
                if (std::isspace(current) || current == '#')
                    break;

                (*position)++;
            }

            if (*position <= start)
                return false;

            *out_token = content.substr(start, *position - start);
            return !out_token->empty();
        }

        static bool draw_load_pbm_content(const std::string &content) {
            size_t position = 0;
            std::string width_token;
            std::string height_token;
            char *end_ptr = NULL;
            long width = 0;
            long height = 0;
            size_t row_bytes = 0;
            int copy_w = 0;
            int copy_h = 0;

            if (content.size() < 3)
                return false;

            if (!(content[0] == 'P' && content[1] == '4'))
                return false;

            position = 2;

            if (!draw_read_pbm_token(content, &position, &width_token))
                return false;

            if (!draw_read_pbm_token(content, &position, &height_token))
                return false;

            width = std::strtol(width_token.c_str(), &end_ptr, 10);
            if (!end_ptr || *end_ptr != '\0' || width <= 0)
                return false;

            height = std::strtol(height_token.c_str(), &end_ptr, 10);
            if (!end_ptr || *end_ptr != '\0' || height <= 0)
                return false;

            if (position >= content.size())
                return false;

            if (!std::isspace(static_cast<unsigned char>(content[position])))
                return false;

            position++;

            row_bytes = static_cast<size_t>((width + 7) / 8);
            if ((position + (row_bytes * static_cast<size_t>(height))) > content.size())
                return false;

            draw_ensure_initialized();
            std::fill(draw_pixels.begin(), draw_pixels.end(), 0);

            copy_w = std::min(draw_canvas_w, static_cast<int>(width));
            copy_h = std::min(draw_canvas_h, static_cast<int>(height));

            for (int y = 0; y < copy_h; y++) {
                const size_t row_start = position + (row_bytes * static_cast<size_t>(y));

                for (int x = 0; x < copy_w; x++) {
                    const size_t src_index = row_start + static_cast<size_t>(x / 8);
                    const unsigned char src_byte = static_cast<unsigned char>(content[src_index]);
                    const unsigned char bit_mask = static_cast<unsigned char>(0x80 >> (x % 8));

                    draw_pixels[static_cast<size_t>((y * draw_canvas_w) + x)] = ((src_byte & bit_mask) != 0) ? 1 : 0;
                }
            }

            draw_dirty_texture = true;
            draw_has_unsaved = false;
            draw_exit_armed = false;
            draw_prev_valid = false;
            return true;
        }

        static bool draw_inside_rect(int x, int y, int x1, int y1, int x2, int y2) {
            return (x >= x1) && (x <= x2) && (y >= y1) && (y <= y2);
        }

        static bool draw_inside_canvas(int x, int y) {
            return draw_inside_rect(x, y,
                draw_canvas_x, draw_canvas_y,
                draw_canvas_x + draw_canvas_w - 1,
                draw_canvas_y + draw_canvas_h - 1);
        }

        static void draw_mark_changed(void) {
            draw_has_unsaved = true;
            draw_exit_armed = false;
        }

        static void draw_set_pixel(int x, int y, bool black) {
            const size_t index = static_cast<size_t>(y * draw_canvas_w + x);
            const u8 next_value = black ? 1 : 0;

            if ((x < 0) || (y < 0) || (x >= draw_canvas_w) || (y >= draw_canvas_h))
                return;

            if (draw_pixels[index] == next_value)
                return;

            draw_pixels[index] = next_value;
            draw_dirty_texture = true;
            draw_mark_changed();
        }

        static void draw_line(int x0, int y0, int x1, int y1, bool black) {
            int dx = std::abs(x1 - x0);
            int sx = (x0 < x1) ? 1 : -1;
            int dy = -std::abs(y1 - y0);
            int sy = (y0 < y1) ? 1 : -1;
            int err = dx + dy;

            while (true) {
                draw_set_pixel(x0, y0, black);

                if ((x0 == x1) && (y0 == y1))
                    break;

                const int e2 = err * 2;
                if (e2 >= dy) {
                    err += dy;
                    x0 += sx;
                }
                if (e2 <= dx) {
                    err += dx;
                    y0 += sy;
                }
            }
        }

        static void draw_clear_canvas(void) {
            bool had_black = false;

            for (size_t i = 0; i < draw_pixels.size(); i++) {
                if (draw_pixels[i] != 0) {
                    had_black = true;
                    break;
                }
            }

            if (!had_black) {
                draw_set_message("Canvas already clear", 1800);
                return;
            }

            std::fill(draw_pixels.begin(), draw_pixels.end(), 0);
            draw_dirty_texture = true;
            draw_mark_changed();
            draw_set_message("Canvas cleared", 1800);
        }

        static std::string draw_resolve_save_path(const std::string &input_name) {
            std::string path = input_name;

            if (path.empty())
                return path;

            if (path.front() != '/') {
                if (!cfg.cwd.empty() && cfg.cwd.back() == '/')
                    path = cfg.cwd + path;
                else
                    path = cfg.cwd + "/" + path;
            }

            if (!path.empty() && path.back() == '/')
                path.append(draw_default_name);

            if (FS::GetFileExt(path) != ".PBM")
                path.append(".pbm");

            return path;
        }

        static std::string draw_serialize_pbm(void) {
            const int row_bytes = (draw_canvas_w + 7) / 8;
            char header[64] = { 0 };
            const int header_len = std::snprintf(header, sizeof(header), "P4\n%d %d\n", draw_canvas_w, draw_canvas_h);
            std::string output;

            output.reserve(static_cast<size_t>(header_len + (row_bytes * draw_canvas_h)));
            output.append(header, static_cast<size_t>(header_len));

            for (int y = 0; y < draw_canvas_h; y++) {
                for (int byte = 0; byte < row_bytes; byte++) {
                    u8 packed = 0;

                    for (int bit = 0; bit < 8; bit++) {
                        const int x = (byte * 8) + bit;

                        if (x >= draw_canvas_w)
                            continue;

                        if (draw_pixels[static_cast<size_t>(y * draw_canvas_w + x)] != 0)
                            packed |= static_cast<u8>(0x80 >> bit);
                    }

                    output.push_back(static_cast<char>(packed));
                }
            }

            return output;
        }

        static void draw_refresh_directory(MenuItem *item) {
            if (item == nullptr)
                return;

            if (R_SUCCEEDED(FS::GetDirList(cfg.cwd, item->entries))) {
                if (item->entries.empty())
                    item->selected = 0;
                else
                    Utils::SetBounds(&item->selected, 0, static_cast<int>(item->entries.size() - 1));

                ResetCheckbox(item);
            }
        }

        static void draw_save(MenuItem *item) {
            const std::string input_name = OSK::GetText(draw_default_name, "Save drawing (.pbm)");
            std::string path;

            if (input_name.empty()) {
                draw_set_message("Save cancelled", 1600);
                return;
            }

            path = draw_resolve_save_path(input_name);
            if (path.empty()) {
                draw_set_message("Invalid save path", 2200);
                return;
            }

            {
                const std::string pbm_data = draw_serialize_pbm();
                const Result save_result = FS::WriteFileFromString(path, pbm_data);

                if (R_FAILED(save_result)) {
                    draw_set_message("Save failed", 2200);
                    return;
                }
            }

            draw_has_unsaved = false;
            draw_exit_armed = false;
            draw_default_name = std::filesystem::path(path).filename().string();

            draw_refresh_directory(item);
            draw_set_message(std::string("Saved: ") + draw_default_name + " | Project->Git to commit", 3200);
        }

        static void draw_switch_tool(DrawTool next_tool) {
            draw_tool = next_tool;

            if (draw_tool == DRAW_TOOL_PEN)
                draw_set_message("Tool: pen", 1500);
            else
                draw_set_message("Tool: eraser", 1500);
        }

        static void draw_touch_button(MenuItem *item, int x, int y) {
            if (!draw_inside_rect(x, y, 8, draw_button_y1, 60, draw_button_y2) &&
                !draw_inside_rect(x, y, 68, draw_button_y1, 120, draw_button_y2) &&
                !draw_inside_rect(x, y, 128, draw_button_y1, 180, draw_button_y2) &&
                !draw_inside_rect(x, y, 188, draw_button_y1, 240, draw_button_y2) &&
                !draw_inside_rect(x, y, 248, draw_button_y1, 312, draw_button_y2)) {
                return;
            }

            if (draw_inside_rect(x, y, 8, draw_button_y1, 60, draw_button_y2))
                draw_save(item);
            else if (draw_inside_rect(x, y, 68, draw_button_y1, 120, draw_button_y2))
                draw_switch_tool(DRAW_TOOL_PEN);
            else if (draw_inside_rect(x, y, 128, draw_button_y1, 180, draw_button_y2))
                draw_switch_tool(DRAW_TOOL_ERASER);
            else if (draw_inside_rect(x, y, 188, draw_button_y1, 240, draw_button_y2))
                draw_clear_canvas();
            else if (draw_inside_rect(x, y, 248, draw_button_y1, 312, draw_button_y2)) {
                if (draw_has_unsaved && !draw_exit_armed) {
                    draw_exit_armed = true;
                    draw_exit_armed_until = osGetTime() + 2500;
                    draw_set_message("Unsaved changes: tap BACK again to discard", 2500);
                }
                else {
                    draw_exit_armed = false;
                    item->state = MENU_STATE_TEXTREADER;
                }
            }
        }

        static void draw_draw_button(const DrawButton &button, bool active) {
            C2D::Rect(static_cast<float>(button.x1), static_cast<float>(draw_button_y1),
                static_cast<float>((button.x2 - button.x1) + 1), static_cast<float>((draw_button_y2 - draw_button_y1) + 1),
                active ? (cfg.dark_theme ? TITLE_COLOUR_DARK : TITLE_COLOUR)
                    : (cfg.dark_theme ? SELECTOR_COLOUR_DARK : SELECTOR_COLOUR_LIGHT));

            C2D::Textf(static_cast<float>(button.x1 + 8), static_cast<float>(draw_button_y1 + 5), 0.31f,
                active ? WHITE : (cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT), "%s", button.label);
        }
    }

    void DisplayDrawingTop(MenuItem *item) {
        (void)item;

        draw_ensure_initialized();
        draw_reset_message_if_expired();

        C2D::Rect(0.f, 40.f, 400.f, 200.f, cfg.dark_theme ? BLACK_BG : WHITE);
        C2D::Text(10.f, 48.f, 0.44f, cfg.dark_theme ? WHITE : BLACK, "Draw Mode (B/W)");
        C2D::Text(10.f, 68.f, 0.31f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "Stylus paints in bottom canvas");

        C2D::Textf(10.f, 84.f, 0.31f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "Tool: %s", (draw_tool == DRAW_TOOL_PEN) ? "Pen (black)" : "Eraser (white)");

        C2D::Textf(10.f, 100.f, 0.31f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "Unsaved: %s", draw_has_unsaved ? "yes" : "no");

        C2D::Text(10.f, 124.f, 0.33f, cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT,
            "Commit flow:");
        C2D::Text(10.f, 140.f, 0.31f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "1) Save .pbm");
        C2D::Text(10.f, 154.f, 0.31f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "2) Open Project -> Git");
        C2D::Text(10.f, 168.f, 0.31f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "3) Add all -> Commit staged -> Push");

        C2D::Text(10.f, 192.f, 0.3f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "A:Save  X:Tool  Y:Clear  B:Back");
    }

    void DisplayDrawingBottom(MenuItem *item) {
        (void)item;

        draw_ensure_initialized();
        draw_reset_message_if_expired();

        C2D::Rect(0.f, 20.f, 320.f, 220.f, cfg.dark_theme ? BLACK_BG : WHITE);

        draw_draw_button(draw_buttons[0], false);
        draw_draw_button(draw_buttons[1], draw_tool == DRAW_TOOL_PEN);
        draw_draw_button(draw_buttons[2], draw_tool == DRAW_TOOL_ERASER);
        draw_draw_button(draw_buttons[3], false);
        draw_draw_button(draw_buttons[4], false);

        C2D::Rect(static_cast<float>(draw_canvas_x - 1), static_cast<float>(draw_canvas_y - 1),
            static_cast<float>(draw_canvas_w + 2), static_cast<float>(draw_canvas_h + 2),
            cfg.dark_theme ? TITLE_COLOUR_DARK : TITLE_COLOUR);

        C2D::Rect(static_cast<float>(draw_canvas_x), static_cast<float>(draw_canvas_y),
            static_cast<float>(draw_canvas_w), static_cast<float>(draw_canvas_h), WHITE);

        if (draw_update_texture() && draw_texture_ready)
            C2D::Image(draw_texture, static_cast<float>(draw_canvas_x), static_cast<float>(draw_canvas_y));
        else
            draw_set_message("Canvas texture error", 2200);

        {
            std::string line1 = draw_message.substr(0, draw_message_line_chars);
            std::string line2;

            if (draw_message.length() > draw_message_line_chars)
                line2 = draw_message.substr(draw_message_line_chars, draw_message_line_chars);

            C2D::Textf(8.f, 198.f, 0.29f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
                "%s", line1.c_str());

            if (!line2.empty()) {
                C2D::Textf(8.f, 210.f, 0.29f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
                    "%s", line2.c_str());
            }
        }
    }

    void ControlDrawingView(MenuItem *item, u32 *kDown, u32 *kHeld) {
        bool touch_button_handled = false;

        draw_ensure_initialized();

        if (draw_exit_armed && (osGetTime() >= draw_exit_armed_until))
            draw_exit_armed = false;

        if (*kDown & KEY_A)
            draw_save(item);
        else if (*kDown & KEY_X)
            draw_switch_tool(draw_tool == DRAW_TOOL_PEN ? DRAW_TOOL_ERASER : DRAW_TOOL_PEN);
        else if (*kDown & KEY_Y)
            draw_clear_canvas();
        else if (*kDown & KEY_B) {
            if (draw_has_unsaved && !draw_exit_armed) {
                draw_exit_armed = true;
                draw_exit_armed_until = osGetTime() + 2500;
                draw_set_message("Unsaved changes: press B again to discard", 2500);
            }
            else {
                draw_exit_armed = false;
                draw_prev_valid = false;
                item->state = MENU_STATE_TEXTREADER;
            }
        }

        if (*kDown & KEY_TOUCH) {
            const int touch_x = static_cast<int>(Touch::GetX());
            const int touch_y = static_cast<int>(Touch::GetY());

            if (draw_inside_rect(touch_x, touch_y, 8, draw_button_y1, 312, draw_button_y2)) {
                draw_touch_button(item, touch_x, touch_y);
                touch_button_handled = true;
                draw_prev_valid = false;
            }
        }

        if ((*kHeld & KEY_TOUCH) && !touch_button_handled) {
            const int touch_x = static_cast<int>(Touch::GetX());
            const int touch_y = static_cast<int>(Touch::GetY());

            if (draw_inside_canvas(touch_x, touch_y)) {
                const u16 local_x = static_cast<u16>(touch_x - draw_canvas_x);
                const u16 local_y = static_cast<u16>(touch_y - draw_canvas_y);
                const bool draw_black = (draw_tool == DRAW_TOOL_PEN);

                if (draw_prev_valid)
                    draw_line(draw_prev_x, draw_prev_y, local_x, local_y, draw_black);
                else
                    draw_set_pixel(local_x, local_y, draw_black);

                draw_prev_x = local_x;
                draw_prev_y = local_y;
                draw_prev_valid = true;
            }
            else {
                draw_prev_valid = false;
            }
        }
        else if (!(*kHeld & KEY_TOUCH)) {
            draw_prev_valid = false;
        }
    }

    void DrawingStartNew(void) {
        draw_tool = DRAW_TOOL_PEN;
        draw_default_name = "drawing.pbm";
        draw_reset_canvas(false, "New drawing ready", 1400);
    }

    bool DrawingOpenFile(const std::string &path) {
        std::string content;
        Result read_result;

        if (path.empty()) {
            draw_set_message("Open failed: invalid path", 2200);
            return false;
        }

        draw_ensure_initialized();

        read_result = FS::ReadFileToString(path, content);
        if (R_FAILED(read_result)) {
            draw_set_message("Open failed: cannot read file", 2200);
            return false;
        }

        if (!draw_load_pbm_content(content)) {
            draw_set_message("Open failed: invalid .pbm", 2200);
            return false;
        }

        draw_tool = DRAW_TOOL_PEN;
        draw_default_name = std::filesystem::path(path).filename().string();
        draw_set_message(std::string("Opened: ") + draw_default_name, 1800);
        return true;
    }
}
