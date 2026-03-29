#include <algorithm>

#include "c2d_helper.h"
#include "colours.h"
#include "config.h"
#include "editor/text_editor.h"
#include "gui.h"
#include "osk.h"
#include "touch.h"

namespace GUI {
    static constexpr float tab_y = 40.f;
    static constexpr float tab_h = 18.f;
    static constexpr float editor_y = 58.f;
    static constexpr float editor_line_h = 12.f;
    static constexpr float line_num_w = 34.f;
    static constexpr int visible_lines = 14;

    static u64 repeat_timestamp = 0;

    static std::string ResolveSavePath(const std::string &input, const std::string &fallback_name) {
        std::string path = input;
        if (path.empty())
            return path;

        if (path.front() != '/')
            path = cfg.cwd + path;

        if (!path.empty() && path.back() == '/')
            path.append(fallback_name);

        return path;
    }

    static void SaveActiveDocument(void) {
        TextEditor::Document *doc = TextEditor::GetActiveDocument();
        if (doc == nullptr)
            return;

        if (TextEditor::SaveActive())
            return;

        std::string initial = doc->path.empty() ? doc->title : doc->path;
        std::string save_input = OSK::GetText(initial, "Save file path");
        std::string resolved = ResolveSavePath(save_input, doc->title.empty() ? "untitled.txt" : doc->title);

        if (!resolved.empty())
            TextEditor::SaveActiveAs(resolved);
    }

    static void CreateNewDocument(void) {
        std::string title = OSK::GetText("untitled.txt", "New file name");
        if (title.empty())
            title = "untitled.txt";

        TextEditor::NewFile(title);
    }

    void DisplayTextReaderTop(MenuItem *item) {
        (void)item;

        const std::vector<TextEditor::Document> &docs = TextEditor::GetDocuments();
        int active_index = TextEditor::GetActiveIndex();

        C2D::Rect(0, tab_y, 400, tab_h, cfg.dark_theme ? MENU_BAR_DARK : SELECTOR_COLOUR_LIGHT);

        for (size_t i = 0; i < docs.size(); i++) {
            float x = 4.f + (96.f * static_cast<float>(i));
            float width = 92.f;
            bool active = static_cast<int>(i) == active_index;

            C2D::Rect(x, tab_y + 1.f, width, tab_h - 2.f, active ? (cfg.dark_theme ? TITLE_COLOUR_DARK : TITLE_COLOUR)
                : (cfg.dark_theme ? SELECTOR_COLOUR_DARK : WHITE));

            std::string title = docs[i].title + (docs[i].dirty ? "*" : "");
            C2D::Textf(x + 3.f, tab_y + 3.f, 0.33f, active ? WHITE : (cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT),
                title.length() > 13 ? "%.13s" : "%s", title.c_str());
        }

        C2D::Rect(0, editor_y, 400, 240.f - editor_y, cfg.dark_theme ? BLACK_BG : WHITE);
        C2D::Rect(0, editor_y, line_num_w, 240.f - editor_y, cfg.dark_theme ? MENU_BAR_DARK : SELECTOR_COLOUR_LIGHT);

        TextEditor::Document *doc = TextEditor::GetActiveDocument();
        if (doc == nullptr) {
            C2D::Text(50.f, editor_y + 8.f, 0.45f, cfg.dark_theme ? WHITE : BLACK, "No document open");
            C2D::Text(50.f, editor_y + 24.f, 0.38f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
                "Press Y to create a new file");
            return;
        }

        int start_line = std::max(0, doc->scroll_line);
        for (int i = 0; i < visible_lines; i++) {
            int line_index = start_line + i;
            if (line_index >= static_cast<int>(doc->lines.size()))
                break;

            float y = editor_y + 2.f + (editor_line_h * static_cast<float>(i));
            C2D::Textf(2.f, y, 0.32f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT, "%d", line_index + 1);

            const std::string &line = doc->lines[line_index];
            C2D::Textf(line_num_w + 2.f, y, 0.33f, cfg.dark_theme ? WHITE : BLACK,
                line.length() > 58 ? "%.58s" : "%s", line.c_str());
        }

        int cursor_row = doc->cursor_line - start_line;
        if (cursor_row >= 0 && cursor_row < visible_lines) {
            float cursor_x = line_num_w + 2.f + (static_cast<float>(doc->cursor_col) * 6.1f);
            float cursor_y = editor_y + 2.f + (editor_line_h * static_cast<float>(cursor_row));
            if (cursor_x > 398.f)
                cursor_x = 398.f;

            C2D::Rect(cursor_x, cursor_y, 2.f, 10.f, cfg.dark_theme ? TITLE_COLOUR_DARK : TITLE_COLOUR);
        }
    }

    void DisplayTextReaderBottom(MenuItem *item) {
        (void)item;

        const std::vector<TextEditor::Document> &docs = TextEditor::GetDocuments();
        TextEditor::Document *doc = TextEditor::GetActiveDocument();

        C2D::Rect(0, 20, 320, 220, cfg.dark_theme ? BLACK_BG : WHITE);

        if (doc != nullptr) {
            C2D::Textf(8, 28, 0.4f, cfg.dark_theme ? WHITE : BLACK, "%s%s", doc->title.c_str(), doc->dirty ? " *" : "");
            C2D::Textf(8, 45, 0.36f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
                "Ln %d  Col %d  Tabs %d/4", doc->cursor_line + 1, doc->cursor_col + 1, static_cast<int>(docs.size()));
        }
        else {
            C2D::Text(8, 28, 0.4f, cfg.dark_theme ? WHITE : BLACK, "No active document");
        }

        C2D::Text(8, 64, 0.34f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "A Insert  X Backspace  Y Save  Select Find");
        C2D::Text(8, 79, 0.34f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "L/R Tabs  B Browser  Touch: NEW/UNDO/REDO");

        C2D::Rect(8, 98, 64, 22, cfg.dark_theme ? SELECTOR_COLOUR_DARK : SELECTOR_COLOUR_LIGHT);
        C2D::Rect(78, 98, 64, 22, cfg.dark_theme ? SELECTOR_COLOUR_DARK : SELECTOR_COLOUR_LIGHT);
        C2D::Rect(148, 98, 64, 22, cfg.dark_theme ? SELECTOR_COLOUR_DARK : SELECTOR_COLOUR_LIGHT);
        C2D::Rect(218, 98, 64, 22, cfg.dark_theme ? SELECTOR_COLOUR_DARK : SELECTOR_COLOUR_LIGHT);

        C2D::Text(22, 103, 0.35f, cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT, "NEW");
        C2D::Text(90, 103, 0.35f, cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT, "SAVE");
        C2D::Text(161, 103, 0.35f, cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT, "UNDO");
        C2D::Text(231, 103, 0.35f, cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT, "REDO");

        for (size_t i = 0; i < 4; i++) {
            float x = 8.f + (78.f * static_cast<float>(i));
            bool has_doc = i < docs.size();
            bool active = static_cast<int>(i) == TextEditor::GetActiveIndex();

            C2D::Rect(x, 134.f, 74.f, 34.f, active ? (cfg.dark_theme ? TITLE_COLOUR_DARK : TITLE_COLOUR)
                : (cfg.dark_theme ? SELECTOR_COLOUR_DARK : SELECTOR_COLOUR_LIGHT));

            if (has_doc) {
                std::string title = docs[i].title + (docs[i].dirty ? "*" : "");
                C2D::Textf(x + 3.f, 145.f, 0.31f, active ? WHITE : (cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT),
                    title.length() > 10 ? "%.10s" : "%s", title.c_str());
            }
            else {
                C2D::Text(x + 24.f, 145.f, 0.31f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT, "-");
            }
        }
    }

    void ControlTextReader(MenuItem *item, u32 *kDown, u32 *kHeld) {
        if (TextEditor::HasActiveDocument()) {
            if ((*kDown & KEY_LEFT) || ((*kHeld & KEY_LEFT) && osGetTime() >= repeat_timestamp)) {
                TextEditor::MoveLeft();
                repeat_timestamp = osGetTime() + ((*kDown & KEY_LEFT) ? 180 : 70);
            }
            else if ((*kDown & KEY_RIGHT) || ((*kHeld & KEY_RIGHT) && osGetTime() >= repeat_timestamp)) {
                TextEditor::MoveRight();
                repeat_timestamp = osGetTime() + ((*kDown & KEY_RIGHT) ? 180 : 70);
            }
            else if ((*kDown & KEY_UP) || ((*kHeld & KEY_UP) && osGetTime() >= repeat_timestamp)) {
                TextEditor::MoveUp();
                repeat_timestamp = osGetTime() + ((*kDown & KEY_UP) ? 180 : 70);
            }
            else if ((*kDown & KEY_DOWN) || ((*kHeld & KEY_DOWN) && osGetTime() >= repeat_timestamp)) {
                TextEditor::MoveDown();
                repeat_timestamp = osGetTime() + ((*kDown & KEY_DOWN) ? 180 : 70);
            }
        }

        if (*kDown & KEY_A) {
            std::string insert_text = OSK::GetText("", "Insert text");
            if (!insert_text.empty())
                TextEditor::InsertText(insert_text);
        }
        else if (*kDown & KEY_X) {
            TextEditor::Backspace();
        }
        else if (*kDown & KEY_Y) {
            SaveActiveDocument();
        }
        else if (*kDown & KEY_SELECT) {
            std::string query = OSK::GetText("", "Find text");
            if (!query.empty())
                TextEditor::FindNext(query);
        }
        else if (*kDown & KEY_L) {
            TextEditor::SetActiveIndex(TextEditor::GetActiveIndex() - 1);
        }
        else if (*kDown & KEY_R) {
            TextEditor::SetActiveIndex(TextEditor::GetActiveIndex() + 1);
        }
#ifdef KEY_ZL
        else if (*kDown & KEY_ZL) {
            TextEditor::Undo();
        }
#endif
#ifdef KEY_ZR
        else if (*kDown & KEY_ZR) {
            TextEditor::Redo();
        }
#endif
        else if (*kDown & KEY_B) {
            item->state = MENU_STATE_FILEBROWSER;
        }

        if (*kDown & KEY_TOUCH) {
            if (Touch::Rect(8, 98, 72, 120))
                CreateNewDocument();
            else if (Touch::Rect(78, 98, 142, 120))
                SaveActiveDocument();
            else if (Touch::Rect(148, 98, 212, 120))
                TextEditor::Undo();
            else if (Touch::Rect(218, 98, 282, 120))
                TextEditor::Redo();
            else {
                const std::vector<TextEditor::Document> &docs = TextEditor::GetDocuments();
                for (size_t i = 0; i < docs.size() && i < 4; i++) {
                    u16 x1 = static_cast<u16>(8 + (78 * i));
                    u16 x2 = static_cast<u16>(x1 + 74);
                    if (Touch::Rect(x1, 134, x2, 168)) {
                        TextEditor::SetActiveIndex(static_cast<int>(i));
                        break;
                    }
                }
            }
        }

        TextEditor::EnsureCursorVisible(visible_lines);
    }
}
