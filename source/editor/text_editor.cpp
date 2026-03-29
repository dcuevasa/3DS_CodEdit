#include <algorithm>
#include <filesystem>

#include "editor/text_editor.h"
#include "fs.h"

namespace TextEditor {
    static std::vector<Document> documents;
    static int active_index = -1;

    static constexpr size_t max_tabs = 4;
    static constexpr size_t max_history = 64;

    static Snapshot CaptureSnapshot(const Document &doc) {
        Snapshot snapshot;
        snapshot.lines = doc.lines;
        snapshot.cursor_line = doc.cursor_line;
        snapshot.cursor_col = doc.cursor_col;
        snapshot.scroll_line = doc.scroll_line;
        return snapshot;
    }

    static void RestoreSnapshot(Document *doc, const Snapshot &snapshot) {
        doc->lines = snapshot.lines;
        doc->cursor_line = snapshot.cursor_line;
        doc->cursor_col = snapshot.cursor_col;
        doc->scroll_line = snapshot.scroll_line;

        if (doc->lines.empty())
            doc->lines.emplace_back();

        if (doc->cursor_line < 0)
            doc->cursor_line = 0;
        if (doc->cursor_line >= static_cast<int>(doc->lines.size()))
            doc->cursor_line = static_cast<int>(doc->lines.size()) - 1;

        const int line_size = static_cast<int>(doc->lines[doc->cursor_line].size());
        if (doc->cursor_col < 0)
            doc->cursor_col = 0;
        if (doc->cursor_col > line_size)
            doc->cursor_col = line_size;

        if (doc->scroll_line < 0)
            doc->scroll_line = 0;
    }

    static std::string GetTitleFromPath(const std::string &path) {
        std::string name = std::filesystem::path(path).filename().string();
        return name.empty() ? path : name;
    }

    static std::vector<std::string> SplitByNewline(const std::string &text) {
        std::vector<std::string> lines;
        std::string current;

        for (char ch : text) {
            if (ch == '\r')
                continue;

            if (ch == '\n') {
                lines.push_back(current);
                current.clear();
                continue;
            }

            current.push_back(ch);
        }

        lines.push_back(current);

        if (lines.empty())
            lines.emplace_back();

        return lines;
    }

    static std::string JoinLines(const std::vector<std::string> &lines) {
        std::string text;

        for (size_t i = 0; i < lines.size(); i++) {
            text.append(lines[i]);
            if (i + 1 < lines.size())
                text.push_back('\n');
        }

        return text;
    }

    static Document *GetActiveMutable(void) {
        if (!HasActiveDocument())
            return nullptr;

        return &documents[active_index];
    }

    static void PushUndoSnapshot(Document *doc) {
        doc->undo_stack.push_back(CaptureSnapshot(*doc));

        if (doc->undo_stack.size() > max_history)
            doc->undo_stack.erase(doc->undo_stack.begin());

        doc->redo_stack.clear();
    }

    static void PushDocument(Document &&doc) {
        if (documents.size() >= max_tabs) {
            documents.erase(documents.begin());
            if (active_index > 0)
                active_index--;
        }

        documents.push_back(std::move(doc));
        active_index = static_cast<int>(documents.size()) - 1;
    }

    static int FindDocumentByPath(const std::string &path) {
        for (size_t i = 0; i < documents.size(); i++) {
            if (documents[i].path == path)
                return static_cast<int>(i);
        }

        return -1;
    }

    bool OpenFile(const std::string &path) {
        if (path.empty())
            return false;

        int existing = FindDocumentByPath(path);
        if (existing >= 0) {
            active_index = existing;
            return true;
        }

        std::string content;
        if (R_FAILED(FS::ReadFileToString(path, content)))
            return false;

        Document doc;
        doc.path = path;
        doc.title = GetTitleFromPath(path);
        doc.lines = SplitByNewline(content);
        doc.cursor_line = 0;
        doc.cursor_col = 0;
        doc.scroll_line = 0;
        doc.dirty = false;
        doc.is_new = false;

        PushDocument(std::move(doc));
        return true;
    }

    void NewFile(const std::string &title) {
        Document doc;
        doc.path.clear();
        doc.title = title.empty() ? "untitled.txt" : title;
        doc.lines = { "" };
        doc.cursor_line = 0;
        doc.cursor_col = 0;
        doc.scroll_line = 0;
        doc.dirty = false;
        doc.is_new = true;

        PushDocument(std::move(doc));
    }

    bool SaveActive(void) {
        Document *doc = GetActiveMutable();
        if (doc == nullptr || doc->path.empty())
            return false;

        std::string content = JoinLines(doc->lines);
        if (R_FAILED(FS::WriteFileFromString(doc->path, content)))
            return false;

        doc->dirty = false;
        doc->is_new = false;
        return true;
    }

    bool SaveActiveAs(const std::string &path) {
        Document *doc = GetActiveMutable();
        if (doc == nullptr || path.empty())
            return false;

        doc->path = path;
        doc->title = GetTitleFromPath(path);
        return SaveActive();
    }

    bool HasActiveDocument(void) {
        return (!documents.empty()) && (active_index >= 0) && (active_index < static_cast<int>(documents.size()));
    }

    Document *GetActiveDocument(void) {
        return GetActiveMutable();
    }

    const std::vector<Document> &GetDocuments(void) {
        return documents;
    }

    int GetActiveIndex(void) {
        return active_index;
    }

    void SetActiveIndex(int index) {
        if (documents.empty()) {
            active_index = -1;
            return;
        }

        if (index < 0)
            index = static_cast<int>(documents.size()) - 1;
        else if (index >= static_cast<int>(documents.size()))
            index = 0;

        active_index = index;
    }

    void CloseActiveDocument(void) {
        if (!HasActiveDocument())
            return;

        documents.erase(documents.begin() + active_index);

        if (documents.empty()) {
            active_index = -1;
            return;
        }

        if (active_index >= static_cast<int>(documents.size()))
            active_index = static_cast<int>(documents.size()) - 1;
    }

    void EnsureCursorVisible(int visible_lines) {
        Document *doc = GetActiveMutable();
        if (doc == nullptr || visible_lines <= 0)
            return;

        if (doc->cursor_line < doc->scroll_line)
            doc->scroll_line = doc->cursor_line;

        if (doc->cursor_line >= doc->scroll_line + visible_lines)
            doc->scroll_line = doc->cursor_line - visible_lines + 1;

        if (doc->scroll_line < 0)
            doc->scroll_line = 0;
    }

    void MoveLeft(void) {
        Document *doc = GetActiveMutable();
        if (doc == nullptr)
            return;

        if (doc->cursor_col > 0)
            doc->cursor_col--;
        else if (doc->cursor_line > 0) {
            doc->cursor_line--;
            doc->cursor_col = static_cast<int>(doc->lines[doc->cursor_line].size());
        }
    }

    void MoveRight(void) {
        Document *doc = GetActiveMutable();
        if (doc == nullptr)
            return;

        const int line_size = static_cast<int>(doc->lines[doc->cursor_line].size());
        if (doc->cursor_col < line_size)
            doc->cursor_col++;
        else if (doc->cursor_line + 1 < static_cast<int>(doc->lines.size())) {
            doc->cursor_line++;
            doc->cursor_col = 0;
        }
    }

    void MoveUp(void) {
        Document *doc = GetActiveMutable();
        if (doc == nullptr || doc->cursor_line == 0)
            return;

        doc->cursor_line--;
        const int line_size = static_cast<int>(doc->lines[doc->cursor_line].size());
        doc->cursor_col = std::min(doc->cursor_col, line_size);
    }

    void MoveDown(void) {
        Document *doc = GetActiveMutable();
        if (doc == nullptr || doc->cursor_line + 1 >= static_cast<int>(doc->lines.size()))
            return;

        doc->cursor_line++;
        const int line_size = static_cast<int>(doc->lines[doc->cursor_line].size());
        doc->cursor_col = std::min(doc->cursor_col, line_size);
    }

    void Backspace(void) {
        Document *doc = GetActiveMutable();
        if (doc == nullptr)
            return;

        if (doc->cursor_line == 0 && doc->cursor_col == 0)
            return;

        PushUndoSnapshot(doc);

        std::string &line = doc->lines[doc->cursor_line];
        if (doc->cursor_col > 0) {
            line.erase(static_cast<size_t>(doc->cursor_col - 1), 1);
            doc->cursor_col--;
        }
        else {
            int previous_line = doc->cursor_line - 1;
            int previous_size = static_cast<int>(doc->lines[previous_line].size());
            doc->lines[previous_line].append(line);
            doc->lines.erase(doc->lines.begin() + doc->cursor_line);
            doc->cursor_line = previous_line;
            doc->cursor_col = previous_size;
        }

        if (doc->lines.empty())
            doc->lines.emplace_back();

        doc->dirty = true;
    }

    void InsertText(const std::string &text) {
        Document *doc = GetActiveMutable();
        if (doc == nullptr || text.empty())
            return;

        PushUndoSnapshot(doc);

        std::vector<std::string> parts = SplitByNewline(text);

        std::string &line = doc->lines[doc->cursor_line];
        std::string left = line.substr(0, static_cast<size_t>(doc->cursor_col));
        std::string right = line.substr(static_cast<size_t>(doc->cursor_col));

        if (parts.size() == 1) {
            line = left + parts[0] + right;
            doc->cursor_col += static_cast<int>(parts[0].size());
        }
        else {
            line = left + parts[0];

            int insert_at = doc->cursor_line + 1;
            for (size_t i = 1; i < parts.size(); i++) {
                doc->lines.insert(doc->lines.begin() + insert_at, parts[i]);
                insert_at++;
            }

            int last_index = insert_at - 1;
            doc->lines[last_index].append(right);
            doc->cursor_line = last_index;
            doc->cursor_col = static_cast<int>(parts.back().size());
        }

        doc->dirty = true;
    }

    void InsertNewLine(void) {
        InsertText("\n");
    }

    bool ReplaceCurrentLine(const std::string &text) {
        Document *doc = GetActiveMutable();
        if (doc == nullptr)
            return false;

        PushUndoSnapshot(doc);

        doc->lines[doc->cursor_line] = text;
        if (doc->cursor_col > static_cast<int>(text.size()))
            doc->cursor_col = static_cast<int>(text.size());

        doc->dirty = true;
        return true;
    }

    bool Undo(void) {
        Document *doc = GetActiveMutable();
        if (doc == nullptr || doc->undo_stack.empty())
            return false;

        doc->redo_stack.push_back(CaptureSnapshot(*doc));
        if (doc->redo_stack.size() > max_history)
            doc->redo_stack.erase(doc->redo_stack.begin());

        Snapshot snapshot = doc->undo_stack.back();
        doc->undo_stack.pop_back();
        RestoreSnapshot(doc, snapshot);
        doc->dirty = true;
        return true;
    }

    bool Redo(void) {
        Document *doc = GetActiveMutable();
        if (doc == nullptr || doc->redo_stack.empty())
            return false;

        doc->undo_stack.push_back(CaptureSnapshot(*doc));
        if (doc->undo_stack.size() > max_history)
            doc->undo_stack.erase(doc->undo_stack.begin());

        Snapshot snapshot = doc->redo_stack.back();
        doc->redo_stack.pop_back();
        RestoreSnapshot(doc, snapshot);
        doc->dirty = true;
        return true;
    }

    bool FindNext(const std::string &query) {
        Document *doc = GetActiveMutable();
        if (doc == nullptr || query.empty())
            return false;

        int line_count = static_cast<int>(doc->lines.size());
        int start_line = doc->cursor_line;
        int start_col = doc->cursor_col + 1;

        for (int step = 0; step < line_count; step++) {
            int line_index = (start_line + step) % line_count;
            size_t from = (line_index == start_line) ? static_cast<size_t>(start_col) : 0;

            size_t pos = doc->lines[line_index].find(query, from);
            if (pos != std::string::npos) {
                doc->cursor_line = line_index;
                doc->cursor_col = static_cast<int>(pos);
                EnsureCursorVisible(14);
                return true;
            }
        }

        return false;
    }
}
