#ifndef _3D_SHELL_TEXT_EDITOR_H
#define _3D_SHELL_TEXT_EDITOR_H

#include <string>
#include <vector>

namespace TextEditor {
    struct Snapshot {
        std::vector<std::string> lines;
        int cursor_line = 0;
        int cursor_col = 0;
        int scroll_line = 0;
    };

    typedef struct {
        std::string path;
        std::string title;
        std::vector<std::string> lines;
        int cursor_line = 0;
        int cursor_col = 0;
        int scroll_line = 0;
        bool dirty = false;
        bool is_new = false;
        std::vector<Snapshot> undo_stack;
        std::vector<Snapshot> redo_stack;
    } Document;

    bool OpenFile(const std::string &path);
    void NewFile(const std::string &title);
    bool SaveActive(void);
    bool SaveActiveAs(const std::string &path);

    bool HasActiveDocument(void);
    Document *GetActiveDocument(void);
    const std::vector<Document> &GetDocuments(void);
    int GetActiveIndex(void);
    void SetActiveIndex(int index);
    void CloseActiveDocument(void);

    void MoveLeft(void);
    void MoveRight(void);
    void MoveUp(void);
    void MoveDown(void);
    void Backspace(void);
    void InsertText(const std::string &text);
    void InsertNewLine(void);
    bool ReplaceCurrentLine(const std::string &text);

    bool Undo(void);
    bool Redo(void);
    bool FindNext(const std::string &query);
    void EnsureCursorVisible(int visible_lines);
}

#endif
