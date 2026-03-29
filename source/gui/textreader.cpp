#include <algorithm>
#include <codecvt>
#include <locale>

#include "c2d_helper.h"
#include "colours.h"
#include "config.h"
#include "editor/text_editor.h"
#include "fs.h"
#include "gui.h"
#include "osk.h"
#include "textures.h"
#include "touch.h"
#include "utils.h"

namespace GUI {
    static constexpr float tab_y = 40.f;
    static constexpr float tab_h = 18.f;
    static constexpr float editor_y = 58.f;
    static constexpr float editor_line_h = 12.f;
    static constexpr float line_num_w = 34.f;
    static constexpr int visible_lines = 14;

    static constexpr int sidebar_visible_entries = 8;
    static constexpr int sidebar_row_height = 17;
    static constexpr int sidebar_list_y = 78;

    static constexpr int top_menu_count = 5;
    static const char *top_menu_labels[top_menu_count] = { "Archivo", "Editar", "Buscar", "Ver", "Proyecto" };
    static const float top_menu_x[top_menu_count] = { 8.f, 78.f, 140.f, 206.f, 252.f };
    static const float top_menu_w[top_menu_count] = { 66.f, 58.f, 60.f, 40.f, 78.f };

    static u64 editor_repeat_timestamp = 0;
    static u64 sidebar_repeat_timestamp = 0;
    static int sidebar_start = 0;
    static int top_menu_index = 0;
    static bool top_menu_focus = false;

    static std::string EntryName(const FS_DirectoryEntry &entry) {
        const std::u16string entry_name_utf16 = reinterpret_cast<const char16_t *>(entry.name);
        return std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(entry_name_utf16.data());
    }

    static void EnsureSidebarSelection(MenuItem *item) {
        if (item->entries.empty()) {
            item->selected = 0;
            sidebar_start = 0;
            return;
        }

        Utils::SetBounds(&item->selected, 0, static_cast<int>(item->entries.size() - 1));

        if (item->selected < sidebar_start)
            sidebar_start = item->selected;

        if (item->selected >= (sidebar_start + sidebar_visible_entries))
            sidebar_start = item->selected - sidebar_visible_entries + 1;

        if (sidebar_start < 0)
            sidebar_start = 0;
    }

    static void RefreshSidebar(MenuItem *item) {
        if (R_SUCCEEDED(FS::GetDirList(cfg.cwd, item->entries)))
            EnsureSidebarSelection(item);
    }

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

    static void EditCurrentLineWithOSK(void) {
        TextEditor::Document *doc = TextEditor::GetActiveDocument();
        if (doc == nullptr)
            return;

        std::string initial = doc->lines[doc->cursor_line];
        std::string updated = OSK::GetText(initial, "Edit current line");
        if (!updated.empty())
            TextEditor::ReplaceCurrentLine(updated);
    }

    static void CloseActiveTab(void) {
        if (!TextEditor::HasActiveDocument())
            return;

        TextEditor::CloseActiveDocument();
        if (!TextEditor::HasActiveDocument())
            TextEditor::NewFile("untitled.txt");
    }

    static void FindText(void) {
        std::string query = OSK::GetText("", "Find text");
        if (!query.empty())
            TextEditor::FindNext(query);
    }

    static void CreateFileEntry(MenuItem *item) {
        std::string filename = OSK::GetText("new_file.txt", "Create file");
        if (filename.empty())
            return;

        std::string path = cfg.cwd;
        path.append(filename);
        std::u16string path_u16 = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(path.data());

        if (R_SUCCEEDED(FSUSER_CreateFile(archive, fsMakePath(PATH_UTF16, path_u16.c_str()), 0, 0))) {
            RefreshSidebar(item);
            TextEditor::OpenFile(path);
        }
    }

    static void CreateFolderEntry(MenuItem *item) {
        std::string dirname = OSK::GetText("new_folder", "Create folder");
        if (dirname.empty())
            return;

        std::string path = cfg.cwd;
        path.append(dirname);
        std::u16string path_u16 = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(path.data());

        if (R_SUCCEEDED(FSUSER_CreateDirectory(archive, fsMakePath(PATH_UTF16, path_u16.c_str()), 0)))
            RefreshSidebar(item);
    }

    static void OpenSidebarSelection(MenuItem *item) {
        if (item->entries.empty())
            return;

        EnsureSidebarSelection(item);
        FS_DirectoryEntry *entry = &item->entries[item->selected];
        std::string filename = EntryName(*entry);

        if (entry->attributes & FS_ATTRIBUTE_DIRECTORY) {
            if (R_SUCCEEDED(FS::ChangeDirNext(filename, item->entries))) {
                item->selected = 0;
                sidebar_start = 0;
            }
            return;
        }

        if (FS::GetFileType(filename) == FileTypeText) {
            std::string path = cfg.cwd;
            path.append(filename);
            TextEditor::OpenFile(path);
        }
    }

    static void GoToParentDir(MenuItem *item) {
        if (R_SUCCEEDED(FS::ChangeDirPrev(item->entries))) {
            item->selected = 0;
            sidebar_start = 0;
        }
    }

    static void DrawSidebarActionButton(int index, const char *label) {
        const int button_w = 60;
        const int button_h = 20;
        const int gap = 3;
        const int x = 4 + (index * (button_w + gap));

        C2D::Rect(static_cast<float>(x), 24.f, static_cast<float>(button_w), static_cast<float>(button_h),
            cfg.dark_theme ? SELECTOR_COLOUR_DARK : SELECTOR_COLOUR_LIGHT);
        C2D::Textf(static_cast<float>(x + 6), 29.f, 0.33f, cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT, "%s", label);
    }

    static void ExecuteTopMenuAction(MenuItem *item) {
        switch (top_menu_index) {
            case 0: // Archivo
                SaveActiveDocument();
                break;

            case 1: // Editar
                EditCurrentLineWithOSK();
                break;

            case 2: // Buscar
                FindText();
                break;

            case 3: // Ver
                TextEditor::InsertNewLine();
                break;

            case 4: // Proyecto
                item->state = MENU_STATE_FILEBROWSER;
                break;

            default:
                break;
        }
    }

    void DisplayTextReaderMenuBar(void) {
        for (int i = 0; i < top_menu_count; i++) {
            bool active = top_menu_focus && (i == top_menu_index);

            if (active) {
                C2D::Rect(top_menu_x[i] - 2.f, 17.f, top_menu_w[i], 20.f,
                    cfg.dark_theme ? TITLE_COLOUR_DARK : TITLE_COLOUR);
            }

            C2D::Text(top_menu_x[i], 21.f, 0.4f, WHITE, top_menu_labels[i]);
        }

        C2D::Text(331.f, 22.f, 0.31f, WHITE, top_menu_focus ? "A/B" : "SELECT");
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

            std::string title = docs[i].title;
            if (docs[i].dirty) {
                if (title.length() > 12)
                    title.resize(12);

                title.append("*");
            }

            C2D::Textf(x + 3.f, tab_y + 3.f, 0.33f, active ? WHITE : (cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT),
                title.length() > 13 ? "%.13s" : "%s", title.c_str());
        }

        C2D::Rect(0, editor_y, 400, 240.f - editor_y, cfg.dark_theme ? BLACK_BG : WHITE);
        C2D::Rect(0, editor_y, line_num_w, 240.f - editor_y, cfg.dark_theme ? MENU_BAR_DARK : SELECTOR_COLOUR_LIGHT);

        TextEditor::Document *doc = TextEditor::GetActiveDocument();
        if (doc == nullptr) {
            C2D::Text(50.f, editor_y + 8.f, 0.45f, cfg.dark_theme ? WHITE : BLACK, "No document open");
            C2D::Text(50.f, editor_y + 24.f, 0.38f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
                "Use +FILE below to create a new file");
            return;
        }

        int start_line = std::max(0, doc->scroll_line);
        for (int i = 0; i < visible_lines; i++) {
            int line_index = start_line + i;
            if (line_index >= static_cast<int>(doc->lines.size()))
                break;

            float y = editor_y + 2.f + (editor_line_h * static_cast<float>(i));

            if (line_index == doc->cursor_line) {
                C2D::Rect(line_num_w + 1.f, y - 1.f, 365.f, editor_line_h,
                    cfg.dark_theme ? SELECTOR_COLOUR_DARK : SELECTOR_COLOUR_LIGHT);
            }

            C2D::Textf(2.f, y, 0.32f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT, "%d", line_index + 1);

            const std::string &line = doc->lines[line_index];
            C2D::Textf(line_num_w + 2.f, y, 0.33f, cfg.dark_theme ? WHITE : BLACK,
                line.length() > 58 ? "%.58s" : "%s", line.c_str());
        }

        int cursor_row = doc->cursor_line - start_line;
        if (cursor_row >= 0 && cursor_row < visible_lines) {
            const std::string &cursor_line_text = doc->lines[doc->cursor_line];
            int visible_col = std::min(doc->cursor_col, 58);
            std::string prefix = cursor_line_text.substr(0, static_cast<size_t>(visible_col));

            float prefix_width = 0.f;
            C2D::GetTextSize(0.33f, &prefix_width, nullptr, prefix.c_str());

            float cursor_x = line_num_w + 2.f + prefix_width;
            float cursor_y = editor_y + 2.f + (editor_line_h * static_cast<float>(cursor_row));
            if (cursor_x > 398.f)
                cursor_x = 398.f;

            C2D::Rect(cursor_x, cursor_y, 2.f, 10.f, cfg.dark_theme ? TITLE_COLOUR_DARK : TITLE_COLOUR);
        }

        C2D::Textf(214.f, 224.f, 0.3f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "Ln %d, Col %d", doc->cursor_line + 1, doc->cursor_col + 1);
    }

    void DisplayTextReaderBottom(MenuItem *item) {
        C2D::Rect(0, 20, 320, 220, cfg.dark_theme ? BLACK_BG : WHITE);

        DrawSidebarActionButton(0, "SAVE");
        DrawSidebarActionButton(1, "+FILE");
        DrawSidebarActionButton(2, "+DIR");
        DrawSidebarActionButton(3, "FIND");
        DrawSidebarActionButton(4, "UP");

        C2D::Textf(6, 50, 0.3f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            cfg.cwd.length() > 42 ? "%.42s..." : "%s", cfg.cwd.c_str());

        if (top_menu_focus) {
            C2D::Textf(6, 63, 0.29f, cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT,
                "Menu %s: A ejecutar | B cerrar", top_menu_labels[top_menu_index]);
        }
        else {
            C2D::Text(6, 63, 0.29f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
                "CirclePad cursor  DPad U/D archivos  <- subir");
        }

        EnsureSidebarSelection(item);

        for (int row = 0; row < sidebar_visible_entries; row++) {
            int index = sidebar_start + row;
            if (index >= static_cast<int>(item->entries.size()))
                break;

            float y = static_cast<float>(sidebar_list_y + (row * sidebar_row_height));
            FS_DirectoryEntry &entry = item->entries[index];
            std::string name = EntryName(entry);
            bool selected = (index == item->selected);

            if (selected)
                C2D::Rect(0, y, 320, static_cast<float>(sidebar_row_height), cfg.dark_theme ? TITLE_COLOUR_DARK : TITLE_COLOUR);

            if (entry.attributes & FS_ATTRIBUTE_DIRECTORY)
                C2D::Image(cfg.dark_theme ? icon_dir_dark : icon_dir, 4.f, y + 1.f);
            else
                C2D::Image(file_icons[FS::GetFileType(name)], 4.f, y + 1.f);

            C2D::Textf(24.f, y + 2.f, 0.33f, selected ? WHITE : (cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT),
                name.length() > 40 ? "%.40s" : "%s", name.c_str());
        }

        if (top_menu_focus) {
            C2D::Text(6, 203, 0.29f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
                "Izq/Der: menu  A: ejecutar  B: cerrar");
        }
        else {
            C2D::Text(6, 203, 0.29f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
                "A abrir  B borrar  X editar  Y salto  -> cerrar");
        }
    }

    void ControlTextReader(MenuItem *item, u32 *kDown, u32 *kHeld) {
        EnsureSidebarSelection(item);

        circlePosition cpad;
        hidCircleRead(&cpad);
        bool stick_active = (cpad.dx > 35) || (cpad.dx < -35) || (cpad.dy > 35) || (cpad.dy < -35);

        if (*kDown & KEY_SELECT)
            top_menu_focus = !top_menu_focus;

        if (top_menu_focus) {
            if ((*kDown & KEY_LEFT) && !stick_active)
                top_menu_index = (top_menu_index + top_menu_count - 1) % top_menu_count;
            else if ((*kDown & KEY_RIGHT) && !stick_active)
                top_menu_index = (top_menu_index + 1) % top_menu_count;
            else if (*kDown & KEY_A) {
                ExecuteTopMenuAction(item);
                top_menu_focus = false;
            }
            else if (*kDown & KEY_B)
                top_menu_focus = false;
        }
        else {
            if (TextEditor::HasActiveDocument()) {
                if ((*kDown & KEY_CPAD_LEFT) || ((*kHeld & KEY_CPAD_LEFT) && osGetTime() >= editor_repeat_timestamp)) {
                    TextEditor::MoveLeft();
                    editor_repeat_timestamp = osGetTime() + ((*kDown & KEY_CPAD_LEFT) ? 180 : 70);
                }
                else if ((*kDown & KEY_CPAD_RIGHT) || ((*kHeld & KEY_CPAD_RIGHT) && osGetTime() >= editor_repeat_timestamp)) {
                    TextEditor::MoveRight();
                    editor_repeat_timestamp = osGetTime() + ((*kDown & KEY_CPAD_RIGHT) ? 180 : 70);
                }
                else if ((*kDown & KEY_CPAD_UP) || ((*kHeld & KEY_CPAD_UP) && osGetTime() >= editor_repeat_timestamp)) {
                    TextEditor::MoveUp();
                    editor_repeat_timestamp = osGetTime() + ((*kDown & KEY_CPAD_UP) ? 180 : 70);
                }
                else if ((*kDown & KEY_CPAD_DOWN) || ((*kHeld & KEY_CPAD_DOWN) && osGetTime() >= editor_repeat_timestamp)) {
                    TextEditor::MoveDown();
                    editor_repeat_timestamp = osGetTime() + ((*kDown & KEY_CPAD_DOWN) ? 180 : 70);
                }
            }

            if (!stick_active && ((*kDown & KEY_UP) || ((*kHeld & KEY_UP) && osGetTime() >= sidebar_repeat_timestamp))) {
                item->selected--;
                sidebar_repeat_timestamp = osGetTime() + ((*kDown & KEY_UP) ? 180 : 70);
                EnsureSidebarSelection(item);
            }
            else if (!stick_active && ((*kDown & KEY_DOWN) || ((*kHeld & KEY_DOWN) && osGetTime() >= sidebar_repeat_timestamp))) {
                item->selected++;
                sidebar_repeat_timestamp = osGetTime() + ((*kDown & KEY_DOWN) ? 180 : 70);
                EnsureSidebarSelection(item);
            }

            if (*kDown & KEY_A) {
                OpenSidebarSelection(item);
            }
            else if (*kDown & KEY_B) {
                TextEditor::Backspace();
            }
            else if ((*kDown & KEY_LEFT) && !stick_active) {
                GoToParentDir(item);
            }
            else if ((*kDown & KEY_RIGHT) && !stick_active) {
                CloseActiveTab();
            }
            else if (*kDown & KEY_X) {
                EditCurrentLineWithOSK();
            }
            else if (*kDown & KEY_Y) {
                TextEditor::InsertNewLine();
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
        }

        if (*kDown & KEY_TOUCH) {
            if (Touch::Rect(4, 24, 64, 44))
                SaveActiveDocument();
            else if (Touch::Rect(67, 24, 127, 44))
                CreateFileEntry(item);
            else if (Touch::Rect(130, 24, 190, 44))
                CreateFolderEntry(item);
            else if (Touch::Rect(193, 24, 253, 44))
                FindText();
            else if (Touch::Rect(256, 24, 316, 44))
                GoToParentDir(item);
            else {
                for (int row = 0; row < sidebar_visible_entries; row++) {
                    int index = sidebar_start + row;
                    if (index >= static_cast<int>(item->entries.size()))
                        break;

                    const int y1 = sidebar_list_y + (row * sidebar_row_height);
                    const int y2 = y1 + sidebar_row_height - 1;
                    if (Touch::Rect(0, static_cast<u16>(y1), 320, static_cast<u16>(y2))) {
                        item->selected = index;
                        OpenSidebarSelection(item);
                        break;
                    }
                }
            }
        }

        TextEditor::EnsureCursorVisible(visible_lines);
    }
}
