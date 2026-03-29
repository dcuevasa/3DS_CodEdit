#include <3ds.h>

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <jansson.h>

#include "git/git.h"

#define CODEDIT_REMOTE_URL_FILE ".git/codedit_remote_url"
#define CODEDIT_REMOTE_BRANCH_FILE ".git/codedit_remote_branch"
#define CODEDIT_REMOTE_HEAD_FILE ".git/codedit_remote_head"

typedef struct {
    char *data;
    size_t size;
} HttpBuffer;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} StringList;

typedef struct {
    char base_rel[GIT_PATH_MAX];
    char pattern[GIT_PATH_MAX];
    bool negate;
    bool directory_only;
    bool anchored;
} IgnoreRule;

typedef struct {
    IgnoreRule *items;
    size_t count;
    size_t capacity;
} IgnoreRuleList;

static void set_message(char *out_message, size_t out_message_len, const char *message) {
    if (!out_message || out_message_len == 0)
        return;

    if (!message)
        message = "";

    snprintf(out_message, out_message_len, "%s", message);
}

static bool is_blank_text(const char *text) {
    size_t i;

    if (!text)
        return true;

    for (i = 0; text[i] != '\0'; i++) {
        if (!isspace((unsigned char)text[i]))
            return false;
    }

    return true;
}

static void set_zero_oid(char *out_head_oid, size_t out_head_oid_len) {
    size_t i;

    if (!out_head_oid || out_head_oid_len < (GIT_OID_HEX_LEN + 1))
        return;

    for (i = 0; i < GIT_OID_HEX_LEN; i++)
        out_head_oid[i] = '0';

    out_head_oid[GIT_OID_HEX_LEN] = '\0';
}

static bool oid_hex_is_valid(const char *oid_hex) {
    size_t i;

    if (!oid_hex || strlen(oid_hex) != GIT_OID_HEX_LEN)
        return false;

    for (i = 0; i < GIT_OID_HEX_LEN; i++) {
        if (!isxdigit((unsigned char)oid_hex[i]))
            return false;
    }

    return true;
}

static bool oid_is_zero(const char *oid_hex) {
    size_t i;

    if (!oid_hex_is_valid(oid_hex))
        return false;

    for (i = 0; i < GIT_OID_HEX_LEN; i++) {
        if (oid_hex[i] != '0')
            return false;
    }

    return true;
}

static char *duplicate_string(const char *text) {
    size_t length;
    char *copy;

    if (!text)
        return NULL;

    length = strlen(text);
    copy = (char *)malloc(length + 1);
    if (!copy)
        return NULL;

    memcpy(copy, text, length + 1);
    return copy;
}

static bool normalize_dir_path(const char *path, char *out_path, size_t out_path_len) {
    size_t length;

    if (!path || !out_path || out_path_len < 2)
        return false;

    length = strnlen(path, out_path_len - 1);
    if (length == 0 || length >= out_path_len)
        return false;

    memcpy(out_path, path, length);
    out_path[length] = '\0';

    if (out_path[0] != '/')
        return false;

    while (length > 1 && out_path[length - 1] == '/') {
        out_path[length - 1] = '\0';
        length--;
    }

    return true;
}

static int path_depth(const char *path) {
    int depth = 0;
    bool in_component = false;
    size_t i;

    if (!path || path[0] != '/')
        return -1;

    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            in_component = false;
        }
        else if (!in_component) {
            in_component = true;
            depth++;
        }
    }

    return depth;
}

static bool is_unsafe_destructive_root(const char *path) {
    char normalized[GIT_PATH_MAX];
    int depth;

    if (!normalize_dir_path(path, normalized, sizeof(normalized)))
        return true;

    if (strcmp(normalized, "/") == 0)
        return true;

    depth = path_depth(normalized);
    if (depth < 2)
        return true;

    return false;
}

static bool join_path(const char *base, const char *leaf, char *out_path, size_t out_path_len) {
    int written;

    if (!base || !leaf || !out_path || out_path_len < 2)
        return false;

    while (*leaf == '/')
        leaf++;

    if (strcmp(base, "/") == 0)
        written = snprintf(out_path, out_path_len, "/%s", leaf);
    else
        written = snprintf(out_path, out_path_len, "%s/%s", base, leaf);

    return written > 0 && (size_t)written < out_path_len;
}

static void parent_dir(char *path) {
    size_t length;

    if (!path)
        return;

    length = strlen(path);
    if (length <= 1) {
        snprintf(path, 2, "/");
        return;
    }

    while (length > 1 && path[length - 1] == '/') {
        path[length - 1] = '\0';
        length--;
    }

    while (length > 1 && path[length - 1] != '/') {
        path[length - 1] = '\0';
        length--;
    }

    while (length > 1 && path[length - 1] == '/') {
        path[length - 1] = '\0';
        length--;
    }

    if (length == 0)
        snprintf(path, 2, "/");
}

static bool open_sdmc_archive(FS_Archive *archive) {
    return archive && R_SUCCEEDED(FSUSER_OpenArchive(archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")));
}

static void close_sdmc_archive(FS_Archive archive) {
    FSUSER_CloseArchive(archive);
}

static GitResult directory_has_entries(FS_Archive archive, const char *path, bool *out_has_entries) {
    Handle dir = 0;
    FS_DirectoryEntry entry;
    u32 entry_count = 0;

    if (!path || !out_has_entries)
        return GIT_RESULT_INVALID_ARG;

    *out_has_entries = false;

    if (R_FAILED(FSUSER_OpenDirectory(&dir, archive, fsMakePath(PATH_ASCII, path))))
        return GIT_RESULT_NOT_FOUND;

    if (R_FAILED(FSDIR_Read(dir, &entry_count, 1, &entry))) {
        FSDIR_Close(dir);
        return GIT_RESULT_IO_ERROR;
    }

    FSDIR_Close(dir);
    *out_has_entries = (entry_count > 0);
    return GIT_RESULT_OK;
}

static bool directory_exists(FS_Archive archive, const char *path) {
    Handle dir = 0;

    if (!path)
        return false;

    if (R_FAILED(FSUSER_OpenDirectory(&dir, archive, fsMakePath(PATH_ASCII, path))))
        return false;

    FSDIR_Close(dir);
    return true;
}

static bool file_exists(FS_Archive archive, const char *path) {
    Handle file = 0;

    if (!path)
        return false;

    if (R_FAILED(FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0)))
        return false;

    FSFILE_Close(file);
    return true;
}

static GitResult ensure_directory(FS_Archive archive, const char *path) {
    Result ret;

    if (!path)
        return GIT_RESULT_INVALID_ARG;

    if (directory_exists(archive, path))
        return GIT_RESULT_OK;

    ret = FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, path), 0);
    if (R_FAILED(ret) && !directory_exists(archive, path))
        return GIT_RESULT_IO_ERROR;

    return GIT_RESULT_OK;
}

static GitResult ensure_directory_recursive(FS_Archive archive, const char *path) {
    char current[GIT_PATH_MAX];
    size_t i;

    if (!normalize_dir_path(path, current, sizeof(current)))
        return GIT_RESULT_INVALID_ARG;

    if (strcmp(current, "/") == 0)
        return GIT_RESULT_OK;

    for (i = 1; current[i] != '\0'; i++) {
        if (current[i] == '/') {
            char saved = current[i];
            GitResult result;

            current[i] = '\0';
            result = ensure_directory(archive, current);
            current[i] = saved;

            if (result != GIT_RESULT_OK)
                return result;
        }
    }

    return ensure_directory(archive, current);
}

static GitResult write_file_bytes(FS_Archive archive, const char *path, const unsigned char *data, size_t size, bool overwrite) {
    Handle file = 0;
    u64 offset = 0;

    if (!path)
        return GIT_RESULT_INVALID_ARG;

    if (overwrite)
        FSUSER_DeleteFile(archive, fsMakePath(PATH_ASCII, path));

    if (!file_exists(archive, path)) {
        if (R_FAILED(FSUSER_CreateFile(archive, fsMakePath(PATH_ASCII, path), 0, (u64)size)))
            return GIT_RESULT_IO_ERROR;
    }

    if (R_FAILED(FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, path), FS_OPEN_WRITE, 0)))
        return GIT_RESULT_IO_ERROR;

    if (R_FAILED(FSFILE_SetSize(file, (u64)size))) {
        FSFILE_Close(file);
        return GIT_RESULT_IO_ERROR;
    }

    while (offset < (u64)size) {
        u64 remaining = (u64)size - offset;
        u32 chunk = (u32)((remaining > 0x10000ULL) ? 0x10000ULL : remaining);
        u32 written = 0;

        if (R_FAILED(FSFILE_Write(file, &written, offset, data + (size_t)offset, chunk, FS_WRITE_FLUSH)) || written != chunk) {
            FSFILE_Close(file);
            return GIT_RESULT_IO_ERROR;
        }

        offset += written;
    }

    FSFILE_Close(file);
    return GIT_RESULT_OK;
}

static GitResult read_file_alloc(FS_Archive archive, const char *path, unsigned char **out_data, size_t *out_size) {
    Handle file = 0;
    u64 size64 = 0;
    unsigned char *buffer = NULL;
    u64 offset = 0;

    if (!path || !out_data || !out_size)
        return GIT_RESULT_INVALID_ARG;

    *out_data = NULL;
    *out_size = 0;

    if (R_FAILED(FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0)))
        return GIT_RESULT_NOT_FOUND;

    if (R_FAILED(FSFILE_GetSize(file, &size64))) {
        FSFILE_Close(file);
        return GIT_RESULT_IO_ERROR;
    }

    if (size64 > (u64)(SIZE_MAX - 1)) {
        FSFILE_Close(file);
        return GIT_RESULT_IO_ERROR;
    }

    buffer = (unsigned char *)malloc((size64 > 0) ? ((size_t)size64 + 1) : 1);
    if (!buffer) {
        FSFILE_Close(file);
        return GIT_RESULT_IO_ERROR;
    }

    while (offset < size64) {
        u64 remaining = size64 - offset;
        u32 chunk = (u32)((remaining > 0x10000ULL) ? 0x10000ULL : remaining);
        u32 read_bytes = 0;

        if (R_FAILED(FSFILE_Read(file, &read_bytes, offset, buffer + (size_t)offset, chunk)) || read_bytes != chunk) {
            FSFILE_Close(file);
            free(buffer);
            return GIT_RESULT_IO_ERROR;
        }

        offset += read_bytes;
    }

    FSFILE_Close(file);
    buffer[(size_t)size64] = '\0';

    *out_data = buffer;
    *out_size = (size_t)size64;
    return GIT_RESULT_OK;
}

static GitResult write_text_file(FS_Archive archive, const char *path, const char *text, bool overwrite) {
    if (!text)
        text = "";

    return write_file_bytes(archive, path, (const unsigned char *)text, strlen(text), overwrite);
}

static GitResult read_text_file(FS_Archive archive, const char *path, char *out_text, size_t out_text_len) {
    unsigned char *data = NULL;
    size_t size = 0;
    GitResult result;

    if (!out_text || out_text_len == 0)
        return GIT_RESULT_INVALID_ARG;

    out_text[0] = '\0';

    result = read_file_alloc(archive, path, &data, &size);
    if (result != GIT_RESULT_OK)
        return result;

    if (size >= out_text_len) {
        memcpy(out_text, data, out_text_len - 1);
        out_text[out_text_len - 1] = '\0';
        free(data);
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    memcpy(out_text, data, size);
    out_text[size] = '\0';
    free(data);
    return GIT_RESULT_OK;
}

static void trim_line_end(char *text) {
    size_t length;

    if (!text)
        return;

    length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r' || text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[length - 1] = '\0';
        length--;
    }
}

static void utf16_to_ascii_lossy(const u16 *input, char *out, size_t out_len) {
    size_t i = 0;

    if (!out || out_len == 0)
        return;

    out[0] = '\0';
    if (!input)
        return;

    while (input[i] != 0 && i + 1 < out_len) {
        u16 ch = input[i];
        out[i] = (ch < 0x80) ? (char)ch : '_';
        i++;
    }

    out[i] = '\0';
}

static GitResult delete_path_recursive(FS_Archive archive, const char *path, bool delete_self) {
    Handle dir = 0;
    Result ret;

    if (!path)
        return GIT_RESULT_INVALID_ARG;

    ret = FSUSER_OpenDirectory(&dir, archive, fsMakePath(PATH_ASCII, path));
    if (R_FAILED(ret)) {
        if (R_FAILED(FSUSER_DeleteFile(archive, fsMakePath(PATH_ASCII, path))))
            return GIT_RESULT_IO_ERROR;
        return GIT_RESULT_OK;
    }

    while (true) {
        FS_DirectoryEntry entry;
        u32 entry_count = 0;
        char name[384];
        char child_path[GIT_PATH_MAX];
        GitResult result;

        if (R_FAILED(FSDIR_Read(dir, &entry_count, 1, &entry))) {
            FSDIR_Close(dir);
            return GIT_RESULT_IO_ERROR;
        }

        if (entry_count == 0)
            break;

        utf16_to_ascii_lossy(entry.name, name, sizeof(name));
        if (name[0] == '\0')
            continue;

        if (!join_path(path, name, child_path, sizeof(child_path))) {
            FSDIR_Close(dir);
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }

        if (entry.attributes & FS_ATTRIBUTE_DIRECTORY)
            result = delete_path_recursive(archive, child_path, true);
        else if (R_FAILED(FSUSER_DeleteFile(archive, fsMakePath(PATH_ASCII, child_path))))
            result = GIT_RESULT_IO_ERROR;
        else
            result = GIT_RESULT_OK;

        if (result != GIT_RESULT_OK) {
            FSDIR_Close(dir);
            return result;
        }
    }

    FSDIR_Close(dir);

    if (delete_self && R_FAILED(FSUSER_DeleteDirectory(archive, fsMakePath(PATH_ASCII, path))))
        return GIT_RESULT_IO_ERROR;

    return GIT_RESULT_OK;
}

static GitResult clear_worktree_except_git(FS_Archive archive, const char *repo_root) {
    Handle dir = 0;

    if (!repo_root)
        return GIT_RESULT_INVALID_ARG;

    if (is_unsafe_destructive_root(repo_root))
        return GIT_RESULT_INVALID_ARG;

    if (R_FAILED(FSUSER_OpenDirectory(&dir, archive, fsMakePath(PATH_ASCII, repo_root))))
        return GIT_RESULT_IO_ERROR;

    while (true) {
        FS_DirectoryEntry entry;
        u32 entry_count = 0;
        char name[384];
        char child_path[GIT_PATH_MAX];
        GitResult result;

        if (R_FAILED(FSDIR_Read(dir, &entry_count, 1, &entry))) {
            FSDIR_Close(dir);
            return GIT_RESULT_IO_ERROR;
        }

        if (entry_count == 0)
            break;

        utf16_to_ascii_lossy(entry.name, name, sizeof(name));
        if (name[0] == '\0')
            continue;

        if (strcmp(name, ".git") == 0)
            continue;

        if (!join_path(repo_root, name, child_path, sizeof(child_path))) {
            FSDIR_Close(dir);
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }

        if (entry.attributes & FS_ATTRIBUTE_DIRECTORY)
            result = delete_path_recursive(archive, child_path, true);
        else if (R_FAILED(FSUSER_DeleteFile(archive, fsMakePath(PATH_ASCII, child_path))))
            result = GIT_RESULT_IO_ERROR;
        else
            result = GIT_RESULT_OK;

        if (result != GIT_RESULT_OK) {
            FSDIR_Close(dir);
            return result;
        }
    }

    FSDIR_Close(dir);
    return GIT_RESULT_OK;
}

static void string_list_init(StringList *list) {
    if (!list)
        return;

    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void string_list_free(StringList *list) {
    size_t i;

    if (!list)
        return;

    for (i = 0; i < list->count; i++)
        free(list->items[i]);

    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static GitResult string_list_push(StringList *list, const char *text) {
    char *copy;

    if (!list || !text)
        return GIT_RESULT_INVALID_ARG;

    if (list->count == list->capacity) {
        size_t new_capacity = (list->capacity == 0) ? 32 : (list->capacity * 2);
        char **new_items = (char **)realloc(list->items, new_capacity * sizeof(char *));

        if (!new_items)
            return GIT_RESULT_IO_ERROR;

        list->items = new_items;
        list->capacity = new_capacity;
    }

    copy = duplicate_string(text);
    if (!copy)
        return GIT_RESULT_IO_ERROR;

    list->items[list->count++] = copy;
    return GIT_RESULT_OK;
}

static void ignore_rule_list_init(IgnoreRuleList *list) {
    if (!list)
        return;

    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void ignore_rule_list_free(IgnoreRuleList *list) {
    if (!list)
        return;

    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void ignore_rule_list_truncate(IgnoreRuleList *list, size_t count) {
    if (!list)
        return;

    if (count < list->count)
        list->count = count;
}

static GitResult ignore_rule_list_push(IgnoreRuleList *list, const IgnoreRule *rule) {
    if (!list || !rule)
        return GIT_RESULT_INVALID_ARG;

    if (list->count == list->capacity) {
        size_t new_capacity = (list->capacity == 0) ? 32 : (list->capacity * 2);
        IgnoreRule *new_items = (IgnoreRule *)realloc(list->items, new_capacity * sizeof(IgnoreRule));

        if (!new_items)
            return GIT_RESULT_IO_ERROR;

        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count++] = *rule;
    return GIT_RESULT_OK;
}

static bool rel_path_starts_with_dir(const char *rel_path, const char *dir_rel, const char **out_subpath) {
    size_t dir_len;

    if (!rel_path || !dir_rel || !out_subpath)
        return false;

    if (dir_rel[0] == '\0') {
        *out_subpath = rel_path;
        return true;
    }

    dir_len = strlen(dir_rel);
    if (strncmp(rel_path, dir_rel, dir_len) != 0)
        return false;

    if (rel_path[dir_len] == '\0') {
        *out_subpath = "";
        return true;
    }

    if (rel_path[dir_len] != '/')
        return false;

    *out_subpath = rel_path + dir_len + 1;
    return true;
}

static const char *path_basename(const char *path) {
    const char *slash;

    if (!path)
        return "";

    slash = strrchr(path, '/');
    return slash ? (slash + 1) : path;
}

static bool gitignore_glob_match(const char *pattern, const char *text) {
    if (!pattern || !text)
        return false;

    while (*pattern != '\0') {
        if (pattern[0] == '*' && pattern[1] == '*') {
            pattern += 2;

            while (*pattern == '*')
                pattern++;

            if (*pattern == '\0')
                return true;

            for (; *text != '\0'; text++) {
                if (gitignore_glob_match(pattern, text))
                    return true;
            }

            return gitignore_glob_match(pattern, text);
        }

        if (*pattern == '*') {
            pattern++;

            while (true) {
                if (gitignore_glob_match(pattern, text))
                    return true;

                if (*text == '\0' || *text == '/')
                    break;

                text++;
            }

            return false;
        }

        if (*pattern == '?') {
            if (*text == '\0' || *text == '/')
                return false;

            pattern++;
            text++;
            continue;
        }

        if (*pattern != *text)
            return false;

        pattern++;
        text++;
    }

    return *text == '\0';
}

static bool gitignore_rule_matches(const IgnoreRule *rule, const char *rel_path, bool is_dir) {
    const char *subpath = NULL;
    bool has_slash;

    if (!rule || !rel_path)
        return false;

    if (rule->directory_only && !is_dir)
        return false;

    if (!rel_path_starts_with_dir(rel_path, rule->base_rel, &subpath))
        return false;

    if (subpath[0] == '\0')
        return false;

    has_slash = strchr(rule->pattern, '/') != NULL;

    if (rule->anchored || has_slash)
        return gitignore_glob_match(rule->pattern, subpath);

    return gitignore_glob_match(rule->pattern, path_basename(subpath));
}

static bool gitignore_is_ignored(const IgnoreRuleList *rules, const char *rel_path, bool is_dir) {
    bool ignored = false;
    size_t i;

    if (!rules || !rel_path)
        return false;

    for (i = 0; i < rules->count; i++) {
        if (gitignore_rule_matches(&rules->items[i], rel_path, is_dir))
            ignored = !rules->items[i].negate;
    }

    return ignored;
}

static GitResult load_gitignore_rules_for_dir(FS_Archive archive, const char *current_abs, const char *current_rel,
    IgnoreRuleList *rules) {
    char gitignore_path[GIT_PATH_MAX];
    unsigned char *data = NULL;
    size_t size = 0;
    size_t line_start = 0;
    size_t i;
    GitResult result;

    if (!current_abs || !current_rel || !rules)
        return GIT_RESULT_INVALID_ARG;

    if (!join_path(current_abs, ".gitignore", gitignore_path, sizeof(gitignore_path)))
        return GIT_RESULT_BUFFER_TOO_SMALL;

    result = read_file_alloc(archive, gitignore_path, &data, &size);
    if (result == GIT_RESULT_NOT_FOUND)
        return GIT_RESULT_OK;

    if (result != GIT_RESULT_OK)
        return result;

    for (i = 0; i <= size; i++) {
        if (i == size || data[i] == '\n') {
            char saved = ((char *)data)[i];
            char *line;

            ((char *)data)[i] = '\0';
            line = (char *)data + line_start;

            while (*line != '\0') {
                size_t len = strlen(line);

                if (len == 0 || line[len - 1] != '\r')
                    break;

                line[len - 1] = '\0';
            }

            if (line[0] != '\0' && line[0] != '#') {
                IgnoreRule rule;
                char *pattern = line;
                size_t pattern_len;

                memset(&rule, 0, sizeof(rule));

                if (snprintf(rule.base_rel, sizeof(rule.base_rel), "%s", current_rel) < 0 ||
                    strlen(rule.base_rel) >= sizeof(rule.base_rel)) {
                    ((char *)data)[i] = saved;
                    free(data);
                    return GIT_RESULT_BUFFER_TOO_SMALL;
                }

                if (pattern[0] == '\\' && (pattern[1] == '#' || pattern[1] == '!')) {
                    pattern++;
                }
                else if (pattern[0] == '!') {
                    rule.negate = true;
                    pattern++;
                }

                if (pattern[0] == '/') {
                    rule.anchored = true;
                    while (pattern[0] == '/')
                        pattern++;
                }

                pattern_len = strlen(pattern);
                while (pattern_len > 0 && pattern[pattern_len - 1] == '/') {
                    rule.directory_only = true;
                    pattern[pattern_len - 1] = '\0';
                    pattern_len--;
                }

                if (pattern_len > 0) {
                    if (snprintf(rule.pattern, sizeof(rule.pattern), "%s", pattern) < 0 ||
                        strlen(rule.pattern) >= sizeof(rule.pattern)) {
                        ((char *)data)[i] = saved;
                        free(data);
                        return GIT_RESULT_BUFFER_TOO_SMALL;
                    }

                    result = ignore_rule_list_push(rules, &rule);
                    if (result != GIT_RESULT_OK) {
                        ((char *)data)[i] = saved;
                        free(data);
                        return result;
                    }
                }
            }

            ((char *)data)[i] = saved;
            line_start = i + 1;
        }
    }

    free(data);
    return GIT_RESULT_OK;
}

static GitResult scan_worktree_files(FS_Archive archive, const char *repo_root,
    const char *current_abs, const char *current_rel, StringList *files, IgnoreRuleList *ignore_rules) {
    Handle dir = 0;
    GitResult result = GIT_RESULT_OK;
    size_t rule_checkpoint = 0;

    if (!repo_root || !current_abs || !current_rel || !files)
        return GIT_RESULT_INVALID_ARG;

    if (ignore_rules)
        rule_checkpoint = ignore_rules->count;

    if (ignore_rules) {
        result = load_gitignore_rules_for_dir(archive, current_abs, current_rel, ignore_rules);
        if (result != GIT_RESULT_OK)
            return result;
    }

    if (R_FAILED(FSUSER_OpenDirectory(&dir, archive, fsMakePath(PATH_ASCII, current_abs))))
        return GIT_RESULT_IO_ERROR;

    while (true) {
        FS_DirectoryEntry entry;
        u32 entry_count = 0;
        char name[384];
        char child_abs[GIT_PATH_MAX];
        char child_rel[GIT_PATH_MAX];
        bool is_dir;

        if (R_FAILED(FSDIR_Read(dir, &entry_count, 1, &entry))) {
            result = GIT_RESULT_IO_ERROR;
            break;
        }

        if (entry_count == 0)
            break;

        utf16_to_ascii_lossy(entry.name, name, sizeof(name));
        if (name[0] == '\0')
            continue;

        if (current_rel[0] == '\0' && strcmp(name, ".git") == 0)
            continue;

        if (!join_path(current_abs, name, child_abs, sizeof(child_abs))) {
            FSDIR_Close(dir);
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }

        if (current_rel[0] == '\0') {
            if (snprintf(child_rel, sizeof(child_rel), "%s", name) <= 0 || strlen(child_rel) >= sizeof(child_rel)) {
                FSDIR_Close(dir);
                return GIT_RESULT_BUFFER_TOO_SMALL;
            }
        }
        else if (snprintf(child_rel, sizeof(child_rel), "%s/%s", current_rel, name) <= 0 || strlen(child_rel) >= sizeof(child_rel)) {
            result = GIT_RESULT_BUFFER_TOO_SMALL;
            break;
        }

        is_dir = (entry.attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
        if (ignore_rules && gitignore_is_ignored(ignore_rules, child_rel, is_dir))
            continue;

        if (is_dir)
            result = scan_worktree_files(archive, repo_root, child_abs, child_rel, files, ignore_rules);
        else
            result = string_list_push(files, child_rel);

        if (result != GIT_RESULT_OK) {
            break;
        }
    }

    FSDIR_Close(dir);

    if (ignore_rules)
        ignore_rule_list_truncate(ignore_rules, rule_checkpoint);

    return result;
}

static size_t write_http_data(void *ptr, size_t size, size_t nmemb, void *userdata) {
    HttpBuffer *buffer = (HttpBuffer *)userdata;
    size_t total_size;
    char *new_data;

    if (!ptr || !buffer)
        return 0;

    total_size = size * nmemb;
    new_data = (char *)realloc(buffer->data, buffer->size + total_size + 1);
    if (!new_data)
        return 0;

    buffer->data = new_data;
    memcpy(buffer->data + buffer->size, ptr, total_size);
    buffer->size += total_size;
    buffer->data[buffer->size] = '\0';
    return total_size;
}

static GitResult github_http_request(const char *method, const char *url, const char *token,
    const char *request_body, const char *accept_header,
    HttpBuffer *out_buffer, long *out_http_code) {
    CURL *handle = NULL;
    CURLcode curl_code;
    struct curl_slist *header_data = NULL;
    const char *effective_method = method ? method : "GET";
    const char *effective_accept = accept_header ? accept_header : "application/vnd.github+json";
    char accept_line[192];

    if (!url || !out_buffer)
        return GIT_RESULT_INVALID_ARG;

    out_buffer->data = NULL;
    out_buffer->size = 0;

    if (out_http_code)
        *out_http_code = 0;

    handle = curl_easy_init();
    if (!handle)
        return GIT_RESULT_IO_ERROR;

    if (snprintf(accept_line, sizeof(accept_line), "Accept: %s", effective_accept) <= 0 ||
        strlen(accept_line) >= sizeof(accept_line)) {
        curl_easy_cleanup(handle);
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    header_data = curl_slist_append(header_data, accept_line);
    header_data = curl_slist_append(header_data, "X-GitHub-Api-Version: 2022-11-28");

    if (request_body)
        header_data = curl_slist_append(header_data, "Content-Type: application/json");

    if (token && token[0] != '\0') {
        char auth_header[768];
        if (snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token) <= 0 ||
            strlen(auth_header) >= sizeof(auth_header)) {
            curl_slist_free_all(header_data);
            curl_easy_cleanup(handle);
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }
        header_data = curl_slist_append(header_data, auth_header);
    }

    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, header_data);
    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "3DS_CodEdit");
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_http_data);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, out_buffer);

    if (strcmp(effective_method, "GET") != 0)
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, effective_method);

    if (request_body) {
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request_body);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, (long)strlen(request_body));
    }

    curl_code = curl_easy_perform(handle);
    if (out_http_code)
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, out_http_code);

    curl_slist_free_all(header_data);
    curl_easy_cleanup(handle);

    if (curl_code != CURLE_OK) {
        free(out_buffer->data);
        out_buffer->data = NULL;
        out_buffer->size = 0;
        return GIT_RESULT_IO_ERROR;
    }

    return GIT_RESULT_OK;
}

static void set_http_error_message(const char *context, long status_code, const char *response_data,
    char *out_message, size_t out_message_len) {
    json_t *root = NULL;
    json_t *message_obj = NULL;
    json_error_t error;
    const char *api_message = NULL;
    char status_text[32];

    if (!context)
        context = "GitHub request failed";

    if (response_data && response_data[0] != '\0') {
        root = json_loads(response_data, JSON_DECODE_ANY, &error);
        if (root) {
            message_obj = json_object_get(root, "message");
            if (json_is_string(message_obj))
                api_message = json_string_value(message_obj);
        }
    }

    if (status_code > 0)
        snprintf(status_text, sizeof(status_text), "HTTP %ld", status_code);
    else
        snprintf(status_text, sizeof(status_text), "HTTP ?");

    if (api_message && api_message[0] != '\0' && strlen(api_message) <= 42) {
        char line[176];
        snprintf(line, sizeof(line), "%s (%s): %s", context, status_text, api_message);
        set_message(out_message, out_message_len, line);
    }
    else {
        char line[128];
        snprintf(line, sizeof(line), "%s (%s)", context, status_text);
        set_message(out_message, out_message_len, line);
    }

    if (root)
        json_decref(root);
}

static GitResult url_encode_text(const char *text, char *out_encoded, size_t out_encoded_len) {
    CURL *handle;
    char *escaped;

    if (!text || !out_encoded || out_encoded_len == 0)
        return GIT_RESULT_INVALID_ARG;

    out_encoded[0] = '\0';

    handle = curl_easy_init();
    if (!handle)
        return GIT_RESULT_IO_ERROR;

    escaped = curl_easy_escape(handle, text, 0);
    if (!escaped) {
        curl_easy_cleanup(handle);
        return GIT_RESULT_IO_ERROR;
    }

    if (snprintf(out_encoded, out_encoded_len, "%s", escaped) <= 0 || strlen(out_encoded) >= out_encoded_len) {
        curl_free(escaped);
        curl_easy_cleanup(handle);
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    curl_free(escaped);
    curl_easy_cleanup(handle);
    return GIT_RESULT_OK;
}

static GitResult url_encode_path_preserving_slash(const char *path, char *out_encoded, size_t out_encoded_len) {
    size_t i;
    size_t j = 0;

    if (!path || !out_encoded || out_encoded_len == 0)
        return GIT_RESULT_INVALID_ARG;

    out_encoded[0] = '\0';

    for (i = 0; path[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)path[i];

        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/') {
            if (j + 1 >= out_encoded_len)
                return GIT_RESULT_BUFFER_TOO_SMALL;

            out_encoded[j++] = (char)ch;
        }
        else {
            if (j + 3 >= out_encoded_len)
                return GIT_RESULT_BUFFER_TOO_SMALL;

            snprintf(out_encoded + j, out_encoded_len - j, "%%%02X", ch);
            j += 3;
        }
    }

    if (j >= out_encoded_len)
        return GIT_RESULT_BUFFER_TOO_SMALL;

    out_encoded[j] = '\0';
    return GIT_RESULT_OK;
}

static bool is_repo_component_valid(const char *value) {
    size_t i;

    if (!value || value[0] == '\0')
        return false;

    for (i = 0; value[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (!(isalnum(ch) || ch == '-' || ch == '_' || ch == '.'))
            return false;
    }

    return true;
}

static bool parse_github_remote_url(const char *remote_url,
    char *out_owner, size_t out_owner_len,
    char *out_repo, size_t out_repo_len) {
    const char *prefix_https = "https://github.com/";
    const char *prefix_http = "http://github.com/";
    const char *start = NULL;
    const char *slash = NULL;
    const char *repo_end = NULL;
    size_t owner_len;
    size_t repo_len;

    if (!remote_url || !out_owner || out_owner_len == 0 || !out_repo || out_repo_len == 0)
        return false;

    if (strncmp(remote_url, prefix_https, strlen(prefix_https)) == 0)
        start = remote_url + strlen(prefix_https);
    else if (strncmp(remote_url, prefix_http, strlen(prefix_http)) == 0)
        start = remote_url + strlen(prefix_http);
    else
        return false;

    while (*start == '/')
        start++;

    slash = strchr(start, '/');
    if (!slash)
        return false;

    owner_len = (size_t)(slash - start);
    if (owner_len == 0 || owner_len >= out_owner_len)
        return false;

    memcpy(out_owner, start, owner_len);
    out_owner[owner_len] = '\0';

    repo_end = slash + 1;
    while (*repo_end != '\0' && *repo_end != '?' && *repo_end != '#')
        repo_end++;

    repo_len = (size_t)(repo_end - (slash + 1));
    while (repo_len > 0 && (slash + 1)[repo_len - 1] == '/')
        repo_len--;

    if (repo_len > 4 && strncmp((slash + 1) + repo_len - 4, ".git", 4) == 0)
        repo_len -= 4;

    if (repo_len == 0 || repo_len >= out_repo_len)
        return false;

    memcpy(out_repo, slash + 1, repo_len);
    out_repo[repo_len] = '\0';

    if (!is_repo_component_valid(out_owner) || !is_repo_component_valid(out_repo))
        return false;

    return true;
}

static GitResult parse_repo_default_branch(const char *json_text,
    char *out_default_branch, size_t out_default_branch_len) {
    json_t *root;
    json_t *branch;
    json_error_t error;
    const char *branch_text;

    if (!json_text || !out_default_branch || out_default_branch_len == 0)
        return GIT_RESULT_INVALID_ARG;

    root = json_loads(json_text, JSON_DECODE_ANY, &error);
    if (!root)
        return GIT_RESULT_PARSE_ERROR;

    branch = json_object_get(root, "default_branch");
    if (json_is_string(branch)) {
        branch_text = json_string_value(branch);
        if (branch_text && branch_text[0] != '\0') {
            if (snprintf(out_default_branch, out_default_branch_len, "%s", branch_text) <= 0 ||
                strlen(out_default_branch) >= out_default_branch_len) {
                json_decref(root);
                return GIT_RESULT_BUFFER_TOO_SMALL;
            }

            json_decref(root);
            return GIT_RESULT_OK;
        }
    }

    if (snprintf(out_default_branch, out_default_branch_len, "%s", "main") <= 0 ||
        strlen(out_default_branch) >= out_default_branch_len) {
        json_decref(root);
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    json_decref(root);
    return GIT_RESULT_OK;
}

static GitResult parse_commit_sha(const char *json_text, char *out_head_oid, size_t out_head_oid_len) {
    json_t *root;
    json_t *sha;
    json_error_t error;
    const char *sha_text;

    if (!json_text || !out_head_oid || out_head_oid_len < (GIT_OID_HEX_LEN + 1))
        return GIT_RESULT_INVALID_ARG;

    root = json_loads(json_text, JSON_DECODE_ANY, &error);
    if (!root)
        return GIT_RESULT_PARSE_ERROR;

    sha = json_object_get(root, "sha");
    if (!json_is_string(sha)) {
        json_decref(root);
        return GIT_RESULT_PARSE_ERROR;
    }

    sha_text = json_string_value(sha);
    if (!sha_text || strlen(sha_text) < GIT_OID_HEX_LEN) {
        json_decref(root);
        return GIT_RESULT_PARSE_ERROR;
    }

    memcpy(out_head_oid, sha_text, GIT_OID_HEX_LEN);
    out_head_oid[GIT_OID_HEX_LEN] = '\0';

    json_decref(root);
    return GIT_RESULT_OK;
}

static GitResult parse_commit_sha_and_tree(const char *json_text,
    char *out_commit_oid, size_t out_commit_oid_len,
    char *out_tree_oid, size_t out_tree_oid_len) {
    json_t *root;
    json_t *sha;
    json_t *commit;
    json_t *tree;
    json_t *tree_sha;
    json_error_t error;
    const char *sha_text;
    const char *tree_text;

    if (!json_text || !out_commit_oid || out_commit_oid_len < (GIT_OID_HEX_LEN + 1) ||
        !out_tree_oid || out_tree_oid_len < (GIT_OID_HEX_LEN + 1)) {
        return GIT_RESULT_INVALID_ARG;
    }

    root = json_loads(json_text, JSON_DECODE_ANY, &error);
    if (!root)
        return GIT_RESULT_PARSE_ERROR;

    sha = json_object_get(root, "sha");
    commit = json_object_get(root, "commit");
    tree = json_is_object(commit) ? json_object_get(commit, "tree") : NULL;
    tree_sha = json_is_object(tree) ? json_object_get(tree, "sha") : NULL;

    if (!json_is_string(sha) || !json_is_string(tree_sha)) {
        json_decref(root);
        return GIT_RESULT_PARSE_ERROR;
    }

    sha_text = json_string_value(sha);
    tree_text = json_string_value(tree_sha);

    if (!sha_text || strlen(sha_text) < GIT_OID_HEX_LEN || !tree_text || strlen(tree_text) < GIT_OID_HEX_LEN) {
        json_decref(root);
        return GIT_RESULT_PARSE_ERROR;
    }

    memcpy(out_commit_oid, sha_text, GIT_OID_HEX_LEN);
    out_commit_oid[GIT_OID_HEX_LEN] = '\0';

    memcpy(out_tree_oid, tree_text, GIT_OID_HEX_LEN);
    out_tree_oid[GIT_OID_HEX_LEN] = '\0';

    json_decref(root);
    return GIT_RESULT_OK;
}

static GitResult parse_top_level_sha(const char *json_text, char *out_sha, size_t out_sha_len) {
    json_t *root;
    json_t *sha;
    json_error_t error;
    const char *sha_text;

    if (!json_text || !out_sha || out_sha_len < (GIT_OID_HEX_LEN + 1))
        return GIT_RESULT_INVALID_ARG;

    root = json_loads(json_text, JSON_DECODE_ANY, &error);
    if (!root)
        return GIT_RESULT_PARSE_ERROR;

    sha = json_object_get(root, "sha");
    if (!json_is_string(sha)) {
        json_decref(root);
        return GIT_RESULT_PARSE_ERROR;
    }

    sha_text = json_string_value(sha);
    if (!sha_text || strlen(sha_text) < GIT_OID_HEX_LEN) {
        json_decref(root);
        return GIT_RESULT_PARSE_ERROR;
    }

    memcpy(out_sha, sha_text, GIT_OID_HEX_LEN);
    out_sha[GIT_OID_HEX_LEN] = '\0';

    json_decref(root);
    return GIT_RESULT_OK;
}

static GitResult github_get_default_branch(const char *owner, const char *repo, const char *token,
    char *out_default_branch, size_t out_default_branch_len,
    char *out_message, size_t out_message_len) {
    char repo_info_url[512];
    HttpBuffer response;
    long status_code;
    GitResult result;

    if (!owner || !repo || !out_default_branch || out_default_branch_len == 0)
        return GIT_RESULT_INVALID_ARG;

    if (snprintf(repo_info_url, sizeof(repo_info_url), "https://api.github.com/repos/%s/%s", owner, repo) <= 0 ||
        strlen(repo_info_url) >= sizeof(repo_info_url)) {
        set_message(out_message, out_message_len, "Repository URL too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    result = github_http_request("GET", repo_info_url, token, NULL, NULL, &response, &status_code);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Network error while querying repository");
        return result;
    }

    if (status_code == 401 || status_code == 403) {
        free(response.data);
        set_message(out_message, out_message_len, "Authentication failed (token) or API rate limit");
        return GIT_RESULT_NOT_FOUND;
    }

    if (status_code == 404) {
        free(response.data);
        set_message(out_message, out_message_len, "Repository not found");
        return GIT_RESULT_NOT_FOUND;
    }

    if (status_code < 200 || status_code >= 300) {
        free(response.data);
        set_message(out_message, out_message_len, "GitHub API request failed");
        return GIT_RESULT_IO_ERROR;
    }

    result = parse_repo_default_branch(response.data, out_default_branch, out_default_branch_len);
    free(response.data);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot parse default branch from GitHub");
        return result;
    }

    return GIT_RESULT_OK;
}

static GitResult github_get_head_and_tree(const char *owner, const char *repo, const char *branch,
    const char *token, char *out_head_oid, size_t out_head_oid_len,
    char *out_tree_oid, size_t out_tree_oid_len,
    bool *out_is_empty_repo,
    char *out_message, size_t out_message_len) {
    char branch_encoded[192];
    char commit_url[640];
    HttpBuffer response;
    long status_code;
    GitResult result;

    if (!owner || !repo || !branch || !out_head_oid || out_head_oid_len < (GIT_OID_HEX_LEN + 1)) {
        set_message(out_message, out_message_len, "Invalid output buffer");
        return GIT_RESULT_INVALID_ARG;
    }

    out_head_oid[0] = '\0';
    if (out_tree_oid && out_tree_oid_len > 0)
        out_tree_oid[0] = '\0';
    if (out_is_empty_repo)
        *out_is_empty_repo = false;

    result = url_encode_text(branch, branch_encoded, sizeof(branch_encoded));
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot encode branch name");
        return result;
    }

    if (snprintf(commit_url, sizeof(commit_url), "https://api.github.com/repos/%s/%s/commits/%s", owner, repo, branch_encoded) <= 0 ||
        strlen(commit_url) >= sizeof(commit_url)) {
        set_message(out_message, out_message_len, "Commit URL too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    result = github_http_request("GET", commit_url, token, NULL, NULL, &response, &status_code);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Network error while querying commit");
        return result;
    }

    if (status_code == 401 || status_code == 403) {
        free(response.data);
        set_message(out_message, out_message_len, "Authentication failed (token) or API rate limit");
        return GIT_RESULT_NOT_FOUND;
    }

    if (status_code == 404) {
        free(response.data);
        set_message(out_message, out_message_len, "Branch not found on remote");
        return GIT_RESULT_NOT_FOUND;
    }

    if (status_code == 409 || status_code == 422) {
        free(response.data);
        set_zero_oid(out_head_oid, out_head_oid_len);
        if (out_tree_oid && out_tree_oid_len > 0)
            out_tree_oid[0] = '\0';
        if (out_is_empty_repo)
            *out_is_empty_repo = true;
        set_message(out_message, out_message_len, "GitHub remote reachable (repository is empty)");
        return GIT_RESULT_OK;
    }

    if (status_code < 200 || status_code >= 300) {
        free(response.data);
        set_message(out_message, out_message_len, "Cannot fetch default branch commit");
        return GIT_RESULT_NOT_FOUND;
    }

    if (out_tree_oid && out_tree_oid_len >= (GIT_OID_HEX_LEN + 1))
        result = parse_commit_sha_and_tree(response.data, out_head_oid, out_head_oid_len, out_tree_oid, out_tree_oid_len);
    else
        result = parse_commit_sha(response.data, out_head_oid, out_head_oid_len);

    free(response.data);

    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot parse commit SHA");
        return result;
    }

    return GIT_RESULT_OK;
}

static void load_remote_state(FS_Archive archive, const char *repo_root,
    char *out_url, size_t out_url_len,
    char *out_branch, size_t out_branch_len,
    char *out_head, size_t out_head_len) {
    char file_path[GIT_PATH_MAX];
    GitResult result;

    if (out_url && out_url_len > 0)
        out_url[0] = '\0';
    if (out_branch && out_branch_len > 0)
        out_branch[0] = '\0';
    if (out_head && out_head_len > 0)
        out_head[0] = '\0';

    if (!repo_root)
        return;

    if (out_url && out_url_len > 0 && join_path(repo_root, CODEDIT_REMOTE_URL_FILE, file_path, sizeof(file_path))) {
        result = read_text_file(archive, file_path, out_url, out_url_len);
        if (result == GIT_RESULT_OK || result == GIT_RESULT_BUFFER_TOO_SMALL)
            trim_line_end(out_url);
        else
            out_url[0] = '\0';
    }

    if (out_branch && out_branch_len > 0 && join_path(repo_root, CODEDIT_REMOTE_BRANCH_FILE, file_path, sizeof(file_path))) {
        result = read_text_file(archive, file_path, out_branch, out_branch_len);
        if (result == GIT_RESULT_OK || result == GIT_RESULT_BUFFER_TOO_SMALL)
            trim_line_end(out_branch);
        else
            out_branch[0] = '\0';
    }

    if (out_head && out_head_len > 0 && join_path(repo_root, CODEDIT_REMOTE_HEAD_FILE, file_path, sizeof(file_path))) {
        result = read_text_file(archive, file_path, out_head, out_head_len);
        if (result == GIT_RESULT_OK || result == GIT_RESULT_BUFFER_TOO_SMALL)
            trim_line_end(out_head);
        else
            out_head[0] = '\0';
    }
}

static GitResult save_remote_state(FS_Archive archive, const char *repo_root,
    const char *remote_url, const char *branch, const char *head_oid) {
    char file_path[GIT_PATH_MAX];
    GitResult result;

    if (!repo_root)
        return GIT_RESULT_INVALID_ARG;

    if (remote_url) {
        if (!join_path(repo_root, CODEDIT_REMOTE_URL_FILE, file_path, sizeof(file_path)))
            return GIT_RESULT_BUFFER_TOO_SMALL;

        result = write_text_file(archive, file_path, remote_url, true);
        if (result != GIT_RESULT_OK)
            return result;
    }

    if (branch) {
        if (!join_path(repo_root, CODEDIT_REMOTE_BRANCH_FILE, file_path, sizeof(file_path)))
            return GIT_RESULT_BUFFER_TOO_SMALL;

        result = write_text_file(archive, file_path, branch, true);
        if (result != GIT_RESULT_OK)
            return result;
    }

    if (head_oid) {
        if (!join_path(repo_root, CODEDIT_REMOTE_HEAD_FILE, file_path, sizeof(file_path)))
            return GIT_RESULT_BUFFER_TOO_SMALL;

        result = write_text_file(archive, file_path, head_oid, true);
        if (result != GIT_RESULT_OK)
            return result;
    }

    return GIT_RESULT_OK;
}

static GitResult write_git_remote_config(FS_Archive archive, const char *repo_root,
    const char *remote_url, const char *branch) {
    char config_path[GIT_PATH_MAX];
    char config_text[4096];

    if (!repo_root || !remote_url || !branch)
        return GIT_RESULT_INVALID_ARG;

    if (!join_path(repo_root, ".git/config", config_path, sizeof(config_path)))
        return GIT_RESULT_BUFFER_TOO_SMALL;

    if (snprintf(config_text, sizeof(config_text),
        "[core]\n"
        "\trepositoryformatversion = 0\n"
        "\tfilemode = true\n"
        "\tbare = false\n"
        "\tlogallrefupdates = true\n"
        "[remote \"origin\"]\n"
        "\turl = %s\n"
        "\tfetch = +refs/heads/*:refs/remotes/origin/*\n"
        "[branch \"%s\"]\n"
        "\tremote = origin\n"
        "\tmerge = refs/heads/%s\n",
        remote_url, branch, branch) <= 0 || strlen(config_text) >= sizeof(config_text)) {
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    return write_text_file(archive, config_path, config_text, true);
}

static GitResult resolve_remote_target(FS_Archive archive, const char *repo_root,
    const char *remote_url_hint, const char *branch_hint, const char *token,
    char *out_remote_url, size_t out_remote_url_len,
    char *out_branch, size_t out_branch_len,
    char *out_owner, size_t out_owner_len,
    char *out_repo, size_t out_repo_len,
    char *out_saved_head, size_t out_saved_head_len,
    char *out_message, size_t out_message_len) {
    char saved_url[GIT_PATH_MAX];
    char saved_branch[128];
    char saved_head[GIT_OID_HEX_LEN + 1];
    GitResult result;

    if (!repo_root || !out_remote_url || out_remote_url_len == 0 || !out_branch || out_branch_len == 0 ||
        !out_owner || out_owner_len == 0 || !out_repo || out_repo_len == 0) {
        set_message(out_message, out_message_len, "Invalid remote target buffer");
        return GIT_RESULT_INVALID_ARG;
    }

    out_remote_url[0] = '\0';
    out_branch[0] = '\0';
    out_owner[0] = '\0';
    out_repo[0] = '\0';
    if (out_saved_head && out_saved_head_len > 0)
        out_saved_head[0] = '\0';

    load_remote_state(archive, repo_root,
        saved_url, sizeof(saved_url),
        saved_branch, sizeof(saved_branch),
        saved_head, sizeof(saved_head));

    if (out_saved_head && out_saved_head_len > 0) {
        snprintf(out_saved_head, out_saved_head_len, "%s", saved_head);
    }

    if (remote_url_hint && remote_url_hint[0] != '\0') {
        if (snprintf(out_remote_url, out_remote_url_len, "%s", remote_url_hint) <= 0 ||
            strlen(out_remote_url) >= out_remote_url_len) {
            set_message(out_message, out_message_len, "Remote URL too long");
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }
    }
    else if (saved_url[0] != '\0') {
        if (snprintf(out_remote_url, out_remote_url_len, "%s", saved_url) <= 0 ||
            strlen(out_remote_url) >= out_remote_url_len) {
            set_message(out_message, out_message_len, "Remote URL too long");
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }
    }
    else {
        set_message(out_message, out_message_len, "No remote configured");
        return GIT_RESULT_NOT_FOUND;
    }

    if (!parse_github_remote_url(out_remote_url, out_owner, out_owner_len, out_repo, out_repo_len)) {
        set_message(out_message, out_message_len, "Invalid GitHub URL");
        return GIT_RESULT_INVALID_ARG;
    }

    if (branch_hint && branch_hint[0] != '\0') {
        if (snprintf(out_branch, out_branch_len, "%s", branch_hint) <= 0 || strlen(out_branch) >= out_branch_len) {
            set_message(out_message, out_message_len, "Branch name too long");
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }
    }
    else if (saved_branch[0] != '\0') {
        if (snprintf(out_branch, out_branch_len, "%s", saved_branch) <= 0 || strlen(out_branch) >= out_branch_len) {
            set_message(out_message, out_message_len, "Branch name too long");
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }
    }
    else {
        result = github_get_default_branch(out_owner, out_repo, token, out_branch, out_branch_len, out_message, out_message_len);
        if (result != GIT_RESULT_OK)
            return result;
    }

    return GIT_RESULT_OK;
}

static GitResult resolve_clone_target_path(FS_Archive archive,
    const char *requested_target,
    const char *repo_name,
    char *out_clone_path,
    size_t out_clone_path_len,
    char *out_message,
    size_t out_message_len) {
    bool child_has_entries = false;
    char git_path[GIT_PATH_MAX];
    char child_path[GIT_PATH_MAX];
    char child_git_path[GIT_PATH_MAX];
    GitResult result;

    if (!requested_target || !repo_name || !out_clone_path || out_clone_path_len == 0)
        return GIT_RESULT_INVALID_ARG;

    out_clone_path[0] = '\0';

    if (!directory_exists(archive, requested_target)) {
        result = ensure_directory_recursive(archive, requested_target);
        if (result != GIT_RESULT_OK) {
            set_message(out_message, out_message_len, "Cannot create target directory");
            return result;
        }
    }

    if (!join_path(requested_target, ".git", git_path, sizeof(git_path))) {
        set_message(out_message, out_message_len, "Target path too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    if (directory_exists(archive, git_path)) {
        set_message(out_message, out_message_len, "Target already has a repository; use Pull");
        return GIT_RESULT_ALREADY_EXISTS;
    }

    if (!join_path(requested_target, repo_name, child_path, sizeof(child_path))) {
        set_message(out_message, out_message_len, "Clone destination path too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    if (!directory_exists(archive, child_path)) {
        result = ensure_directory_recursive(archive, child_path);
        if (result != GIT_RESULT_OK) {
            set_message(out_message, out_message_len, "Cannot create clone destination folder");
            return result;
        }
    }

    if (!join_path(child_path, ".git", child_git_path, sizeof(child_git_path))) {
        set_message(out_message, out_message_len, "Clone destination path too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    if (directory_exists(archive, child_git_path)) {
        set_message(out_message, out_message_len, "Clone destination already has a repository");
        return GIT_RESULT_ALREADY_EXISTS;
    }

    result = directory_has_entries(archive, child_path, &child_has_entries);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot inspect clone destination");
        return result;
    }

    if (child_has_entries) {
        set_message(out_message, out_message_len, "Clone destination is not empty");
        return GIT_RESULT_ALREADY_EXISTS;
    }

    if (snprintf(out_clone_path, out_clone_path_len, "%s", child_path) <= 0 ||
        strlen(out_clone_path) >= out_clone_path_len) {
        set_message(out_message, out_message_len, "Clone destination path too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    return GIT_RESULT_OK;
}

static GitResult apply_remote_snapshot(FS_Archive archive, const char *repo_root,
    const char *owner, const char *repo, const char *branch, const char *token,
    int *out_file_count,
    char *out_message, size_t out_message_len) {
    char branch_encoded[192];
    char tree_url[640];
    HttpBuffer tree_response;
    long status_code;
    GitResult result;
    json_t *root = NULL;
    json_t *tree = NULL;
    size_t index;
    int file_count = 0;

    if (out_file_count)
        *out_file_count = 0;

    if (!repo_root || !owner || !repo || !branch)
        return GIT_RESULT_INVALID_ARG;

    result = url_encode_text(branch, branch_encoded, sizeof(branch_encoded));
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot encode branch name");
        return result;
    }

    if (snprintf(tree_url, sizeof(tree_url), "https://api.github.com/repos/%s/%s/git/trees/%s?recursive=1", owner, repo, branch_encoded) <= 0 ||
        strlen(tree_url) >= sizeof(tree_url)) {
        set_message(out_message, out_message_len, "Tree URL too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    result = github_http_request("GET", tree_url, token, NULL, NULL, &tree_response, &status_code);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Network error while querying tree");
        return result;
    }

    if (status_code == 401 || status_code == 403) {
        free(tree_response.data);
        set_message(out_message, out_message_len, "Authentication failed (token) or API rate limit");
        return GIT_RESULT_NOT_FOUND;
    }

    if (status_code == 404) {
        free(tree_response.data);
        set_message(out_message, out_message_len, "Remote tree not found");
        return GIT_RESULT_NOT_FOUND;
    }

    if (status_code < 200 || status_code >= 300) {
        free(tree_response.data);
        set_message(out_message, out_message_len, "Cannot fetch remote tree");
        return GIT_RESULT_IO_ERROR;
    }

    {
        json_error_t error;
        root = json_loads(tree_response.data ? tree_response.data : "", JSON_DECODE_ANY, &error);
    }
    free(tree_response.data);

    if (!root) {
        set_message(out_message, out_message_len, "Cannot parse remote tree JSON");
        return GIT_RESULT_PARSE_ERROR;
    }

    {
        json_t *truncated = json_object_get(root, "truncated");
        if (json_is_true(truncated)) {
            json_decref(root);
            set_message(out_message, out_message_len, "Remote tree too large (truncated)");
            return GIT_RESULT_IO_ERROR;
        }
    }

    tree = json_object_get(root, "tree");
    if (!json_is_array(tree)) {
        json_decref(root);
        set_message(out_message, out_message_len, "Invalid tree payload");
        return GIT_RESULT_PARSE_ERROR;
    }

    result = clear_worktree_except_git(archive, repo_root);
    if (result != GIT_RESULT_OK) {
        json_decref(root);
        if (result == GIT_RESULT_INVALID_ARG)
            set_message(out_message, out_message_len, "Refusing to clean unsafe repository root");
        else
            set_message(out_message, out_message_len, "Cannot clean working tree");
        return result;
    }

    for (index = 0; index < json_array_size(tree); index++) {
        json_t *entry = json_array_get(tree, index);
        json_t *type_obj = json_object_get(entry, "type");
        json_t *path_obj = json_object_get(entry, "path");
        json_t *sha_obj = json_object_get(entry, "sha");
        const char *type = json_is_string(type_obj) ? json_string_value(type_obj) : NULL;
        const char *path = json_is_string(path_obj) ? json_string_value(path_obj) : NULL;
        const char *sha = json_is_string(sha_obj) ? json_string_value(sha_obj) : NULL;

        if (!type || !path || !sha)
            continue;

        if (strcmp(type, "blob") == 0) {
            char abs_path[GIT_PATH_MAX];
            char parent_path[GIT_PATH_MAX];
            char blob_url[640];
            HttpBuffer blob_response;
            long blob_status = 0;

            if (!join_path(repo_root, path, abs_path, sizeof(abs_path))) {
                json_decref(root);
                set_message(out_message, out_message_len, "File path too long");
                return GIT_RESULT_BUFFER_TOO_SMALL;
            }

            snprintf(parent_path, sizeof(parent_path), "%s", abs_path);
            parent_dir(parent_path);

            result = ensure_directory_recursive(archive, parent_path);
            if (result != GIT_RESULT_OK) {
                json_decref(root);
                set_message(out_message, out_message_len, "Cannot create file directories");
                return result;
            }

            if (snprintf(blob_url, sizeof(blob_url), "https://api.github.com/repos/%s/%s/git/blobs/%s", owner, repo, sha) <= 0 ||
                strlen(blob_url) >= sizeof(blob_url)) {
                json_decref(root);
                set_message(out_message, out_message_len, "Blob URL too long");
                return GIT_RESULT_BUFFER_TOO_SMALL;
            }

            result = github_http_request("GET", blob_url, token, NULL, "application/vnd.github.raw", &blob_response, &blob_status);
            if (result != GIT_RESULT_OK) {
                json_decref(root);
                set_message(out_message, out_message_len, "Network error while downloading blob");
                return result;
            }

            if (blob_status < 200 || blob_status >= 300) {
                free(blob_response.data);
                json_decref(root);
                set_message(out_message, out_message_len, "Cannot download blob from GitHub");
                return GIT_RESULT_IO_ERROR;
            }

            result = write_file_bytes(archive, abs_path,
                (const unsigned char *)(blob_response.data ? blob_response.data : ""),
                blob_response.size,
                true);

            free(blob_response.data);

            if (result != GIT_RESULT_OK) {
                json_decref(root);
                set_message(out_message, out_message_len, "Cannot write file to working tree");
                return result;
            }

            file_count++;
        }
    }

    json_decref(root);

    if (out_file_count)
        *out_file_count = file_count;

    return GIT_RESULT_OK;
}

static char *base64_encode(const unsigned char *data, size_t data_len, size_t *out_len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t encoded_len;
    char *encoded;
    size_t i = 0;
    size_t j = 0;

    if (!data && data_len > 0)
        return NULL;

    encoded_len = ((data_len + 2) / 3) * 4;
    encoded = (char *)malloc(encoded_len + 1);
    if (!encoded)
        return NULL;

    while (i < data_len) {
        uint32_t octet_a = (i < data_len) ? data[i++] : 0;
        uint32_t octet_b = (i < data_len) ? data[i++] : 0;
        uint32_t octet_c = (i < data_len) ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        encoded[j++] = table[(triple >> 18) & 0x3F];
        encoded[j++] = table[(triple >> 12) & 0x3F];
        encoded[j++] = table[(triple >> 6) & 0x3F];
        encoded[j++] = table[triple & 0x3F];
    }

    if (data_len % 3 == 1) {
        encoded[encoded_len - 1] = '=';
        encoded[encoded_len - 2] = '=';
    }
    else if (data_len % 3 == 2) {
        encoded[encoded_len - 1] = '=';
    }

    encoded[encoded_len] = '\0';

    if (out_len)
        *out_len = encoded_len;

    return encoded;
}

static GitResult github_create_blob(const char *owner, const char *repo, const char *token,
    const unsigned char *data, size_t data_len,
    char *out_blob_oid, size_t out_blob_oid_len,
    char *out_message, size_t out_message_len) {
    char url[640];
    char *encoded = NULL;
    size_t encoded_len = 0;
    json_t *body_root = NULL;
    char *body_text = NULL;
    HttpBuffer response;
    long status_code = 0;
    GitResult result;

    if (!owner || !repo || !out_blob_oid || out_blob_oid_len < (GIT_OID_HEX_LEN + 1))
        return GIT_RESULT_INVALID_ARG;

    out_blob_oid[0] = '\0';

    encoded = base64_encode(data, data_len, &encoded_len);
    if (!encoded) {
        set_message(out_message, out_message_len, "Cannot encode blob payload");
        return GIT_RESULT_IO_ERROR;
    }

    body_root = json_object();
    if (!body_root) {
        free(encoded);
        return GIT_RESULT_IO_ERROR;
    }

    json_object_set_new(body_root, "content", json_stringn(encoded, encoded_len));
    json_object_set_new(body_root, "encoding", json_string("base64"));

    body_text = json_dumps(body_root, JSON_COMPACT);
    json_decref(body_root);
    body_root = NULL;
    free(encoded);

    if (!body_text) {
        set_message(out_message, out_message_len, "Cannot serialize blob payload");
        return GIT_RESULT_IO_ERROR;
    }

    if (snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/git/blobs", owner, repo) <= 0 ||
        strlen(url) >= sizeof(url)) {
        free(body_text);
        set_message(out_message, out_message_len, "Blob URL too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    result = github_http_request("POST", url, token, body_text, NULL, &response, &status_code);
    free(body_text);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Network error while creating blob");
        return result;
    }

    if (status_code < 200 || status_code >= 300) {
        if (status_code == 401 || status_code == 403) {
            char message[128];
            snprintf(message, sizeof(message), "Push denied: token needs Contents:write (HTTP %ld)", status_code);
            set_message(out_message, out_message_len, message);
        }
        else
            set_http_error_message("GitHub rejected blob creation", status_code, response.data, out_message, out_message_len);
        free(response.data);
        return GIT_RESULT_IO_ERROR;
    }

    result = parse_top_level_sha(response.data, out_blob_oid, out_blob_oid_len);
    free(response.data);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot parse blob SHA");
        return result;
    }

    return GIT_RESULT_OK;
}

static GitResult github_create_tree(const char *owner, const char *repo, const char *token,
    json_t *tree_entries,
    char *out_tree_oid, size_t out_tree_oid_len,
    char *out_message, size_t out_message_len) {
    char url[640];
    json_t *body_root = NULL;
    char *body_text = NULL;
    HttpBuffer response;
    long status_code = 0;
    GitResult result;

    if (!owner || !repo || !tree_entries || !json_is_array(tree_entries) ||
        !out_tree_oid || out_tree_oid_len < (GIT_OID_HEX_LEN + 1)) {
        return GIT_RESULT_INVALID_ARG;
    }

    out_tree_oid[0] = '\0';

    body_root = json_object();
    if (!body_root)
        return GIT_RESULT_IO_ERROR;

    json_object_set_new(body_root, "tree", json_incref(tree_entries));
    body_text = json_dumps(body_root, JSON_COMPACT);
    json_decref(body_root);

    if (!body_text) {
        set_message(out_message, out_message_len, "Cannot serialize tree payload");
        return GIT_RESULT_IO_ERROR;
    }

    if (snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/git/trees", owner, repo) <= 0 ||
        strlen(url) >= sizeof(url)) {
        free(body_text);
        set_message(out_message, out_message_len, "Tree URL too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    result = github_http_request("POST", url, token, body_text, NULL, &response, &status_code);
    free(body_text);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Network error while creating tree");
        return result;
    }

    if (status_code < 200 || status_code >= 300) {
        set_http_error_message("GitHub rejected tree creation", status_code, response.data, out_message, out_message_len);
        free(response.data);
        return GIT_RESULT_IO_ERROR;
    }

    result = parse_top_level_sha(response.data, out_tree_oid, out_tree_oid_len);
    free(response.data);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot parse tree SHA");
        return result;
    }

    return GIT_RESULT_OK;
}

static GitResult github_create_commit(const char *owner, const char *repo, const char *token,
    const char *commit_message, const char *tree_oid, const char *parent_oid,
    char *out_commit_oid, size_t out_commit_oid_len,
    char *out_message, size_t out_message_len) {
    char url[640];
    json_t *body_root = NULL;
    char *body_text = NULL;
    HttpBuffer response;
    long status_code = 0;
    GitResult result;

    if (!owner || !repo || !commit_message || !tree_oid || !out_commit_oid || out_commit_oid_len < (GIT_OID_HEX_LEN + 1))
        return GIT_RESULT_INVALID_ARG;

    out_commit_oid[0] = '\0';

    body_root = json_object();
    if (!body_root)
        return GIT_RESULT_IO_ERROR;

    json_object_set_new(body_root, "message", json_string(commit_message));
    json_object_set_new(body_root, "tree", json_string(tree_oid));

    if (parent_oid && oid_hex_is_valid(parent_oid) && !oid_is_zero(parent_oid)) {
        json_t *parents = json_array();
        if (!parents) {
            json_decref(body_root);
            return GIT_RESULT_IO_ERROR;
        }

        json_array_append_new(parents, json_string(parent_oid));
        json_object_set_new(body_root, "parents", parents);
    }

    body_text = json_dumps(body_root, JSON_COMPACT);
    json_decref(body_root);

    if (!body_text) {
        set_message(out_message, out_message_len, "Cannot serialize commit payload");
        return GIT_RESULT_IO_ERROR;
    }

    if (snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/git/commits", owner, repo) <= 0 ||
        strlen(url) >= sizeof(url)) {
        free(body_text);
        set_message(out_message, out_message_len, "Commit URL too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    result = github_http_request("POST", url, token, body_text, NULL, &response, &status_code);
    free(body_text);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Network error while creating commit");
        return result;
    }

    if (status_code < 200 || status_code >= 300) {
        set_http_error_message("GitHub rejected commit creation", status_code, response.data, out_message, out_message_len);
        free(response.data);
        return GIT_RESULT_IO_ERROR;
    }

    result = parse_top_level_sha(response.data, out_commit_oid, out_commit_oid_len);
    free(response.data);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot parse commit SHA");
        return result;
    }

    return GIT_RESULT_OK;
}

static GitResult github_update_or_create_ref(const char *owner, const char *repo, const char *branch,
    const char *token, const char *new_commit_oid,
    char *out_message, size_t out_message_len) {
    char branch_encoded[192];
    char patch_url[768];
    char create_url[640];
    json_t *body_root = NULL;
    char *body_text = NULL;
    HttpBuffer response;
    long status_code = 0;
    GitResult result;

    if (!owner || !repo || !branch || !new_commit_oid)
        return GIT_RESULT_INVALID_ARG;

    result = url_encode_text(branch, branch_encoded, sizeof(branch_encoded));
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot encode branch name");
        return result;
    }

    if (snprintf(patch_url, sizeof(patch_url), "https://api.github.com/repos/%s/%s/git/refs/heads/%s", owner, repo, branch_encoded) <= 0 ||
        strlen(patch_url) >= sizeof(patch_url)) {
        set_message(out_message, out_message_len, "Ref URL too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    body_root = json_object();
    if (!body_root)
        return GIT_RESULT_IO_ERROR;

    json_object_set_new(body_root, "sha", json_string(new_commit_oid));
    json_object_set_new(body_root, "force", json_false());
    body_text = json_dumps(body_root, JSON_COMPACT);
    json_decref(body_root);

    if (!body_text)
        return GIT_RESULT_IO_ERROR;

    result = github_http_request("PATCH", patch_url, token, body_text, NULL, &response, &status_code);
    free(body_text);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Network error while updating branch ref");
        return result;
    }

    if (status_code >= 200 && status_code < 300) {
        free(response.data);
        return GIT_RESULT_OK;
    }

    if (status_code != 404) {
        set_http_error_message("Push rejected while updating branch ref", status_code, response.data, out_message, out_message_len);
        free(response.data);
        return GIT_RESULT_IO_ERROR;
    }

    free(response.data);

    if (snprintf(create_url, sizeof(create_url), "https://api.github.com/repos/%s/%s/git/refs", owner, repo) <= 0 ||
        strlen(create_url) >= sizeof(create_url)) {
        set_message(out_message, out_message_len, "Ref URL too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    body_root = json_object();
    if (!body_root)
        return GIT_RESULT_IO_ERROR;

    {
        char ref_name[256];
        if (snprintf(ref_name, sizeof(ref_name), "refs/heads/%s", branch) <= 0 || strlen(ref_name) >= sizeof(ref_name)) {
            json_decref(body_root);
            set_message(out_message, out_message_len, "Branch ref too long");
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }
        json_object_set_new(body_root, "ref", json_string(ref_name));
    }
    json_object_set_new(body_root, "sha", json_string(new_commit_oid));

    body_text = json_dumps(body_root, JSON_COMPACT);
    json_decref(body_root);
    if (!body_text)
        return GIT_RESULT_IO_ERROR;

    result = github_http_request("POST", create_url, token, body_text, NULL, &response, &status_code);
    free(body_text);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Network error while creating branch ref");
        return result;
    }

    if (status_code < 200 || status_code >= 300) {
        set_http_error_message("Cannot create branch ref on GitHub", status_code, response.data, out_message, out_message_len);
        free(response.data);
        return GIT_RESULT_IO_ERROR;
    }

    free(response.data);
    return GIT_RESULT_OK;
}

static GitResult parse_contents_commit_sha(const char *json_text, char *out_commit_oid, size_t out_commit_oid_len) {
    json_t *root;
    json_t *commit;
    json_t *sha;
    json_error_t error;
    const char *sha_text;

    if (!json_text || !out_commit_oid || out_commit_oid_len < (GIT_OID_HEX_LEN + 1))
        return GIT_RESULT_INVALID_ARG;

    root = json_loads(json_text, JSON_DECODE_ANY, &error);
    if (!root)
        return GIT_RESULT_PARSE_ERROR;

    commit = json_object_get(root, "commit");
    sha = json_is_object(commit) ? json_object_get(commit, "sha") : NULL;
    if (!json_is_string(sha)) {
        json_decref(root);
        return GIT_RESULT_PARSE_ERROR;
    }

    sha_text = json_string_value(sha);
    if (!sha_text || strlen(sha_text) < GIT_OID_HEX_LEN) {
        json_decref(root);
        return GIT_RESULT_PARSE_ERROR;
    }

    memcpy(out_commit_oid, sha_text, GIT_OID_HEX_LEN);
    out_commit_oid[GIT_OID_HEX_LEN] = '\0';

    json_decref(root);
    return GIT_RESULT_OK;
}

static GitResult github_put_file_contents(const char *owner, const char *repo, const char *token,
    const char *branch, const char *path,
    const unsigned char *data, size_t data_len,
    const char *commit_message,
    char *out_commit_oid, size_t out_commit_oid_len,
    char *out_message, size_t out_message_len) {
    char encoded_path[2 * GIT_PATH_MAX];
    char url[1024];
    char *encoded_content = NULL;
    size_t encoded_content_len = 0;
    json_t *body_root = NULL;
    char *body_text = NULL;
    HttpBuffer response;
    long status_code = 0;
    GitResult result;

    if (!owner || !repo || !token || !branch || !path || !commit_message ||
        !out_commit_oid || out_commit_oid_len < (GIT_OID_HEX_LEN + 1)) {
        return GIT_RESULT_INVALID_ARG;
    }

    out_commit_oid[0] = '\0';

    result = url_encode_path_preserving_slash(path, encoded_path, sizeof(encoded_path));
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot encode file path for push");
        return result;
    }

    if (snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/contents/%s", owner, repo, encoded_path) <= 0 ||
        strlen(url) >= sizeof(url)) {
        set_message(out_message, out_message_len, "Content URL too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    encoded_content = base64_encode(data, data_len, &encoded_content_len);
    if (!encoded_content) {
        set_message(out_message, out_message_len, "Cannot encode file content for push");
        return GIT_RESULT_IO_ERROR;
    }

    body_root = json_object();
    if (!body_root) {
        free(encoded_content);
        return GIT_RESULT_IO_ERROR;
    }

    json_object_set_new(body_root, "message", json_string(commit_message));
    json_object_set_new(body_root, "content", json_stringn(encoded_content, encoded_content_len));
    json_object_set_new(body_root, "branch", json_string(branch));

    body_text = json_dumps(body_root, JSON_COMPACT);
    json_decref(body_root);
    free(encoded_content);

    if (!body_text) {
        set_message(out_message, out_message_len, "Cannot serialize content push payload");
        return GIT_RESULT_IO_ERROR;
    }

    result = github_http_request("PUT", url, token, body_text, NULL, &response, &status_code);
    free(body_text);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Network error while uploading file content");
        return result;
    }

    if (status_code < 200 || status_code >= 300) {
        if (status_code == 401 || status_code == 403) {
            char message[128];
            snprintf(message, sizeof(message), "Push denied: token needs Contents:write (HTTP %ld)", status_code);
            set_message(out_message, out_message_len, message);
        }
        else {
            set_http_error_message("GitHub rejected content upload", status_code, response.data, out_message, out_message_len);
        }

        free(response.data);
        return GIT_RESULT_IO_ERROR;
    }

    result = parse_contents_commit_sha(response.data, out_commit_oid, out_commit_oid_len);
    free(response.data);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot parse commit SHA from content push");
        return result;
    }

    return GIT_RESULT_OK;
}

static GitResult github_bootstrap_empty_repo_push(FS_Archive archive,
    const char *repo_root,
    const StringList *files,
    const char *owner,
    const char *repo,
    const char *token,
    const char *branch,
    const char *commit_message,
    char *out_head_oid,
    size_t out_head_oid_len,
    char *out_message,
    size_t out_message_len) {
    size_t i;
    char last_commit[GIT_OID_HEX_LEN + 1];

    if (!repo_root || !files || !owner || !repo || !token || !branch || !commit_message ||
        !out_head_oid || out_head_oid_len < (GIT_OID_HEX_LEN + 1)) {
        return GIT_RESULT_INVALID_ARG;
    }

    out_head_oid[0] = '\0';
    last_commit[0] = '\0';

    if (files->count == 0) {
        set_message(out_message, out_message_len, "Empty repository push requires at least one file");
        return GIT_RESULT_INVALID_ARG;
    }

    for (i = 0; i < files->count; i++) {
        const char *rel_path = files->items[i];
        char abs_path[GIT_PATH_MAX];
        unsigned char *file_data = NULL;
        size_t file_size = 0;
        char step_message[192];
        GitResult result;

        if (!join_path(repo_root, rel_path, abs_path, sizeof(abs_path))) {
            set_message(out_message, out_message_len, "File path too long");
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }

        result = read_file_alloc(archive, abs_path, &file_data, &file_size);
        if (result != GIT_RESULT_OK) {
            set_message(out_message, out_message_len, "Cannot read local file for push");
            return result;
        }

        if (files->count == 1)
            snprintf(step_message, sizeof(step_message), "%s", commit_message);
        else
            snprintf(step_message, sizeof(step_message), "%s (%lu/%lu)", commit_message,
                (unsigned long)(i + 1), (unsigned long)files->count);

        result = github_put_file_contents(owner, repo, token, branch, rel_path,
            file_data, file_size,
            step_message,
            last_commit, sizeof(last_commit),
            out_message, out_message_len);

        free(file_data);

        if (result != GIT_RESULT_OK)
            return result;
    }

    if (!oid_hex_is_valid(last_commit)) {
        set_message(out_message, out_message_len, "Cannot determine remote commit after bootstrap push");
        return GIT_RESULT_PARSE_ERROR;
    }

    snprintf(out_head_oid, out_head_oid_len, "%s", last_commit);
    return GIT_RESULT_OK;
}

GitResult git_get_saved_remote_state(const char *start_path,
    char *out_remote_url, size_t out_remote_url_len,
    char *out_branch, size_t out_branch_len,
    char *out_head_oid, size_t out_head_oid_len) {
    FS_Archive archive;
    char repo_root[GIT_PATH_MAX];

    if (!out_remote_url || out_remote_url_len == 0 ||
        !out_branch || out_branch_len == 0 ||
        !out_head_oid || out_head_oid_len < (GIT_OID_HEX_LEN + 1)) {
        return GIT_RESULT_INVALID_ARG;
    }

    out_remote_url[0] = '\0';
    out_branch[0] = '\0';
    out_head_oid[0] = '\0';

    if (!git_find_repository_root(start_path, repo_root, sizeof(repo_root)))
        return GIT_RESULT_NO_REPOSITORY;

    if (!open_sdmc_archive(&archive))
        return GIT_RESULT_IO_ERROR;

    load_remote_state(archive, repo_root,
        out_remote_url, out_remote_url_len,
        out_branch, out_branch_len,
        out_head_oid, out_head_oid_len);

    close_sdmc_archive(archive);

    if (out_remote_url[0] == '\0')
        return GIT_RESULT_NOT_FOUND;

    return GIT_RESULT_OK;
}

GitResult git_set_saved_remote_state(const char *start_path,
    const char *remote_url,
    const char *branch,
    char *out_message, size_t out_message_len) {
    FS_Archive archive;
    char repo_root[GIT_PATH_MAX];
    char owner[128];
    char repo[128];
    GitResult result;

    if (!start_path || !remote_url || !branch || branch[0] == '\0') {
        set_message(out_message, out_message_len, "Invalid remote bind arguments");
        return GIT_RESULT_INVALID_ARG;
    }

    if (!parse_github_remote_url(remote_url, owner, sizeof(owner), repo, sizeof(repo))) {
        set_message(out_message, out_message_len, "Invalid GitHub URL");
        return GIT_RESULT_INVALID_ARG;
    }

    if (!git_find_repository_root(start_path, repo_root, sizeof(repo_root))) {
        set_message(out_message, out_message_len, "No repository found");
        return GIT_RESULT_NO_REPOSITORY;
    }

    if (!open_sdmc_archive(&archive)) {
        set_message(out_message, out_message_len, "Failed to open SD archive");
        return GIT_RESULT_IO_ERROR;
    }

    result = save_remote_state(archive, repo_root, remote_url, branch, NULL);
    if (result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot save repository remote state");
        return result;
    }

    result = write_git_remote_config(archive, repo_root, remote_url, branch);
    close_sdmc_archive(archive);

    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot write repository .git/config remote section");
        return result;
    }

    set_message(out_message, out_message_len, "Remote bound to current repository");
    return GIT_RESULT_OK;
}

GitResult git_remote_probe_github(const char *remote_url, const char *token,
    char *out_default_branch, size_t out_default_branch_len,
    char *out_head_oid, size_t out_head_oid_len,
    char *out_message, size_t out_message_len) {
    char owner[128];
    char repo[128];
    GitResult result;

    if (!out_default_branch || out_default_branch_len == 0 || !out_head_oid ||
        out_head_oid_len < (GIT_OID_HEX_LEN + 1)) {
        set_message(out_message, out_message_len, "Invalid output buffer");
        return GIT_RESULT_INVALID_ARG;
    }

    out_default_branch[0] = '\0';
    out_head_oid[0] = '\0';

    if (!parse_github_remote_url(remote_url, owner, sizeof(owner), repo, sizeof(repo))) {
        set_message(out_message, out_message_len, "Invalid GitHub URL");
        return GIT_RESULT_INVALID_ARG;
    }

    result = github_get_default_branch(owner, repo, token, out_default_branch, out_default_branch_len, out_message, out_message_len);
    if (result != GIT_RESULT_OK)
        return result;

    result = github_get_head_and_tree(owner, repo, out_default_branch, token,
        out_head_oid, out_head_oid_len,
        NULL, 0,
        NULL,
        out_message, out_message_len);
    if (result != GIT_RESULT_OK)
        return result;

    if (oid_is_zero(out_head_oid))
        set_message(out_message, out_message_len, "GitHub remote reachable (repository is empty)");
    else
        set_message(out_message, out_message_len, "GitHub remote reachable");

    return GIT_RESULT_OK;
}

GitResult git_remote_clone_github(const char *target_path, const char *remote_url, const char *branch_hint, const char *token,
    char *out_default_branch, size_t out_default_branch_len,
    char *out_head_oid, size_t out_head_oid_len,
    char *out_message, size_t out_message_len) {
    FS_Archive archive;
    char normalized_target[GIT_PATH_MAX];
    char clone_target[GIT_PATH_MAX];
    char repo_root[GIT_PATH_MAX];
    char owner[128];
    char repo[128];
    char resolved_branch[128];
    char remote_head[GIT_OID_HEX_LEN + 1];
    char remote_tree[GIT_OID_HEX_LEN + 1];
    char init_message[128];
    int file_count = 0;
    GitResult result;

    if (out_default_branch && out_default_branch_len > 0)
        out_default_branch[0] = '\0';
    if (out_head_oid && out_head_oid_len > 0)
        out_head_oid[0] = '\0';

    if (!target_path || !remote_url || !out_default_branch || out_default_branch_len == 0 ||
        !out_head_oid || out_head_oid_len < (GIT_OID_HEX_LEN + 1)) {
        set_message(out_message, out_message_len, "Invalid clone arguments");
        return GIT_RESULT_INVALID_ARG;
    }

    if (!normalize_dir_path(target_path, normalized_target, sizeof(normalized_target))) {
        set_message(out_message, out_message_len, "Invalid target path");
        return GIT_RESULT_INVALID_ARG;
    }

    if (!parse_github_remote_url(remote_url, owner, sizeof(owner), repo, sizeof(repo))) {
        set_message(out_message, out_message_len, "Invalid GitHub URL");
        return GIT_RESULT_INVALID_ARG;
    }

    if (!open_sdmc_archive(&archive)) {
        set_message(out_message, out_message_len, "Failed to open SD archive");
        return GIT_RESULT_IO_ERROR;
    }

    result = resolve_clone_target_path(archive,
        normalized_target,
        repo,
        clone_target,
        sizeof(clone_target),
        out_message,
        out_message_len);
    if (result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        return result;
    }

    result = git_init_repository(clone_target, init_message, sizeof(init_message));
    if (result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot initialize local repository");
        return result;
    }

    if (!git_find_repository_root(clone_target, repo_root, sizeof(repo_root))) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot locate local repository root");
        return GIT_RESULT_NO_REPOSITORY;
    }

    if (branch_hint && branch_hint[0] != '\0') {
        if (snprintf(resolved_branch, sizeof(resolved_branch), "%s", branch_hint) <= 0 || strlen(resolved_branch) >= sizeof(resolved_branch)) {
            close_sdmc_archive(archive);
            set_message(out_message, out_message_len, "Branch name too long");
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }
    }
    else {
        result = github_get_default_branch(owner, repo, token, resolved_branch, sizeof(resolved_branch), out_message, out_message_len);
        if (result != GIT_RESULT_OK) {
            close_sdmc_archive(archive);
            return result;
        }
    }

    result = github_get_head_and_tree(owner, repo, resolved_branch, token,
        remote_head, sizeof(remote_head),
        remote_tree, sizeof(remote_tree),
        NULL,
        out_message, out_message_len);
    if (result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        return result;
    }

    if (!oid_is_zero(remote_head)) {
        result = apply_remote_snapshot(archive, repo_root, owner, repo, resolved_branch, token, &file_count, out_message, out_message_len);
        if (result != GIT_RESULT_OK) {
            close_sdmc_archive(archive);
            if (result == GIT_RESULT_INVALID_ARG)
                set_message(out_message, out_message_len, "Unsafe clone destination for clean operation");
            return result;
        }
    }

    result = save_remote_state(archive, repo_root, remote_url, resolved_branch, remote_head);
    if (result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot save remote state");
        return result;
    }

    result = write_git_remote_config(archive, repo_root, remote_url, resolved_branch);
    if (result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot write .git/config remote section");
        return result;
    }

    close_sdmc_archive(archive);

    snprintf(out_default_branch, out_default_branch_len, "%s", resolved_branch);
    snprintf(out_head_oid, out_head_oid_len, "%s", remote_head);

    if (oid_is_zero(remote_head)) {
        char message[200];
        snprintf(message, sizeof(message), "Clone completed in %s (empty repository)", clone_target);
        set_message(out_message, out_message_len, message);
    }
    else {
        char short_sha[8];
        memcpy(short_sha, remote_head, 7);
        short_sha[7] = '\0';
        {
            char message[208];
            snprintf(message, sizeof(message), "Clone completed in %s: %d file(s), %s", clone_target, file_count, short_sha);
            set_message(out_message, out_message_len, message);
        }
    }

    return GIT_RESULT_OK;
}

GitResult git_remote_fetch_github(const char *start_path, const char *remote_url, const char *branch_hint, const char *token,
    bool *out_has_updates,
    char *out_branch, size_t out_branch_len,
    char *out_head_oid, size_t out_head_oid_len,
    char *out_message, size_t out_message_len) {
    FS_Archive archive;
    char repo_root[GIT_PATH_MAX];
    char resolved_url[GIT_PATH_MAX];
    char resolved_branch[128];
    char owner[128];
    char repo[128];
    char saved_head[GIT_OID_HEX_LEN + 1];
    char remote_head[GIT_OID_HEX_LEN + 1];
    char remote_tree[GIT_OID_HEX_LEN + 1];
    bool has_updates = false;
    GitResult result;

    if (out_has_updates)
        *out_has_updates = false;
    if (out_branch && out_branch_len > 0)
        out_branch[0] = '\0';
    if (out_head_oid && out_head_oid_len > 0)
        out_head_oid[0] = '\0';

    if (!start_path || !out_branch || out_branch_len == 0 || !out_head_oid || out_head_oid_len < (GIT_OID_HEX_LEN + 1)) {
        set_message(out_message, out_message_len, "Invalid fetch arguments");
        return GIT_RESULT_INVALID_ARG;
    }

    if (!git_find_repository_root(start_path, repo_root, sizeof(repo_root))) {
        set_message(out_message, out_message_len, "No repository found");
        return GIT_RESULT_NO_REPOSITORY;
    }

    if (!open_sdmc_archive(&archive)) {
        set_message(out_message, out_message_len, "Failed to open SD archive");
        return GIT_RESULT_IO_ERROR;
    }

    result = resolve_remote_target(archive, repo_root, remote_url, branch_hint, token,
        resolved_url, sizeof(resolved_url),
        resolved_branch, sizeof(resolved_branch),
        owner, sizeof(owner),
        repo, sizeof(repo),
        saved_head, sizeof(saved_head),
        out_message, out_message_len);
    if (result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        return result;
    }

    result = github_get_head_and_tree(owner, repo, resolved_branch, token,
        remote_head, sizeof(remote_head),
        remote_tree, sizeof(remote_tree),
        NULL,
        out_message, out_message_len);
    if (result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        return result;
    }

    if (saved_head[0] == '\0')
        has_updates = !oid_is_zero(remote_head);
    else if (oid_hex_is_valid(saved_head))
        has_updates = (strcmp(saved_head, remote_head) != 0);
    else
        has_updates = true;

    result = save_remote_state(archive, repo_root, resolved_url, resolved_branch, saved_head[0] != '\0' ? saved_head : NULL);
    close_sdmc_archive(archive);
    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot save remote configuration");
        return result;
    }

    if (out_has_updates)
        *out_has_updates = has_updates;
    snprintf(out_branch, out_branch_len, "%s", resolved_branch);
    snprintf(out_head_oid, out_head_oid_len, "%s", remote_head);

    if (has_updates) {
        char short_sha[8];
        memcpy(short_sha, remote_head, 7);
        short_sha[7] = '\0';
        {
            char message[160];
            snprintf(message, sizeof(message), "Fetch: updates available at %s", short_sha);
            set_message(out_message, out_message_len, message);
        }
    }
    else {
        set_message(out_message, out_message_len, "Fetch: already up to date");
    }

    return GIT_RESULT_OK;
}

GitResult git_remote_pull_github(const char *start_path, const char *remote_url, const char *branch_hint, const char *token,
    char *out_branch, size_t out_branch_len,
    char *out_head_oid, size_t out_head_oid_len,
    char *out_message, size_t out_message_len) {
    FS_Archive archive;
    char repo_root[GIT_PATH_MAX];
    char resolved_url[GIT_PATH_MAX];
    char resolved_branch[128];
    char owner[128];
    char repo[128];
    char saved_head[GIT_OID_HEX_LEN + 1];
    char remote_head[GIT_OID_HEX_LEN + 1];
    char remote_tree[GIT_OID_HEX_LEN + 1];
    int file_count = 0;
    bool need_update;
    GitResult result;

    if (out_branch && out_branch_len > 0)
        out_branch[0] = '\0';
    if (out_head_oid && out_head_oid_len > 0)
        out_head_oid[0] = '\0';

    if (!start_path || !out_branch || out_branch_len == 0 || !out_head_oid || out_head_oid_len < (GIT_OID_HEX_LEN + 1)) {
        set_message(out_message, out_message_len, "Invalid pull arguments");
        return GIT_RESULT_INVALID_ARG;
    }

    if (!git_find_repository_root(start_path, repo_root, sizeof(repo_root))) {
        set_message(out_message, out_message_len, "No repository found");
        return GIT_RESULT_NO_REPOSITORY;
    }

    if (!open_sdmc_archive(&archive)) {
        set_message(out_message, out_message_len, "Failed to open SD archive");
        return GIT_RESULT_IO_ERROR;
    }

    result = resolve_remote_target(archive, repo_root, remote_url, branch_hint, token,
        resolved_url, sizeof(resolved_url),
        resolved_branch, sizeof(resolved_branch),
        owner, sizeof(owner),
        repo, sizeof(repo),
        saved_head, sizeof(saved_head),
        out_message, out_message_len);
    if (result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        return result;
    }

    result = github_get_head_and_tree(owner, repo, resolved_branch, token,
        remote_head, sizeof(remote_head),
        remote_tree, sizeof(remote_tree),
        NULL,
        out_message, out_message_len);
    if (result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        return result;
    }

    if (saved_head[0] == '\0')
        need_update = !oid_is_zero(remote_head);
    else if (oid_hex_is_valid(saved_head))
        need_update = (strcmp(saved_head, remote_head) != 0);
    else
        need_update = true;

    if (need_update) {
        if (oid_is_zero(remote_head))
            result = clear_worktree_except_git(archive, repo_root);
        else
            result = apply_remote_snapshot(archive, repo_root, owner, repo, resolved_branch, token, &file_count, out_message, out_message_len);

        if (result != GIT_RESULT_OK) {
            if (result == GIT_RESULT_INVALID_ARG)
                set_message(out_message, out_message_len, "Refusing to clean unsafe repository root");
            close_sdmc_archive(archive);
            return result;
        }
    }

    result = save_remote_state(archive, repo_root, resolved_url, resolved_branch, remote_head);
    if (result == GIT_RESULT_OK)
        result = write_git_remote_config(archive, repo_root, resolved_url, resolved_branch);

    close_sdmc_archive(archive);

    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Cannot save remote state");
        return result;
    }

    snprintf(out_branch, out_branch_len, "%s", resolved_branch);
    snprintf(out_head_oid, out_head_oid_len, "%s", remote_head);

    if (!need_update)
        set_message(out_message, out_message_len, "Pull: already up to date");
    else if (oid_is_zero(remote_head))
        set_message(out_message, out_message_len, "Pull completed: repository is now empty");
    else {
        char short_sha[8];
        memcpy(short_sha, remote_head, 7);
        short_sha[7] = '\0';
        {
            char message[160];
            snprintf(message, sizeof(message), "Pull completed: %d file(s), %s", file_count, short_sha);
            set_message(out_message, out_message_len, message);
        }
    }

    return GIT_RESULT_OK;
}

GitResult git_remote_push_github(const char *start_path, const char *remote_url, const char *branch_hint,
    const char *token, const char *commit_message,
    char *out_branch, size_t out_branch_len,
    char *out_head_oid, size_t out_head_oid_len,
    char *out_message, size_t out_message_len) {
    FS_Archive archive;
    char repo_root[GIT_PATH_MAX];
    char resolved_url[GIT_PATH_MAX];
    char resolved_branch[128];
    char owner[128];
    char repo[128];
    char saved_head[GIT_OID_HEX_LEN + 1];
    char remote_head[GIT_OID_HEX_LEN + 1];
    char remote_tree[GIT_OID_HEX_LEN + 1];
    StringList files;
    IgnoreRuleList ignore_rules;
    json_t *tree_entries = NULL;
    char new_tree_oid[GIT_OID_HEX_LEN + 1];
    char new_commit_oid[GIT_OID_HEX_LEN + 1];
    size_t pushed_count = 0;
    size_t i;
    GitResult result;

    if (out_branch && out_branch_len > 0)
        out_branch[0] = '\0';
    if (out_head_oid && out_head_oid_len > 0)
        out_head_oid[0] = '\0';

    if (!start_path || !commit_message || !out_branch || out_branch_len == 0 ||
        !out_head_oid || out_head_oid_len < (GIT_OID_HEX_LEN + 1)) {
        set_message(out_message, out_message_len, "Invalid push arguments");
        return GIT_RESULT_INVALID_ARG;
    }

    if (is_blank_text(commit_message)) {
        set_message(out_message, out_message_len, "Push commit message is empty");
        return GIT_RESULT_INVALID_ARG;
    }

    if (!token || token[0] == '\0') {
        set_message(out_message, out_message_len, "GitHub token required for push");
        return GIT_RESULT_INVALID_ARG;
    }

    if (!git_find_repository_root(start_path, repo_root, sizeof(repo_root))) {
        set_message(out_message, out_message_len, "No repository found");
        return GIT_RESULT_NO_REPOSITORY;
    }

    if (!open_sdmc_archive(&archive)) {
        set_message(out_message, out_message_len, "Failed to open SD archive");
        return GIT_RESULT_IO_ERROR;
    }

    result = resolve_remote_target(archive, repo_root, remote_url, branch_hint, token,
        resolved_url, sizeof(resolved_url),
        resolved_branch, sizeof(resolved_branch),
        owner, sizeof(owner),
        repo, sizeof(repo),
        saved_head, sizeof(saved_head),
        out_message, out_message_len);
    if (result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        return result;
    }

    result = github_get_head_and_tree(owner, repo, resolved_branch, token,
        remote_head, sizeof(remote_head),
        remote_tree, sizeof(remote_tree),
        NULL,
        out_message, out_message_len);
    if (result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        return result;
    }

    if (saved_head[0] == '\0') {
        if (!oid_is_zero(remote_head)) {
            close_sdmc_archive(archive);
            set_message(out_message, out_message_len, "No local sync baseline. Run Pull before Push");
            return GIT_RESULT_IO_ERROR;
        }
    }
    else if (oid_hex_is_valid(saved_head) && strcmp(saved_head, remote_head) != 0) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Remote advanced. Run Pull before Push");
        return GIT_RESULT_IO_ERROR;
    }

    string_list_init(&files);
    ignore_rule_list_init(&ignore_rules);
    result = scan_worktree_files(archive, repo_root, repo_root, "", &files, &ignore_rules);
    ignore_rule_list_free(&ignore_rules);
    if (result != GIT_RESULT_OK) {
        string_list_free(&files);
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot scan working tree files");
        return result;
    }

    tree_entries = json_array();
    if (!tree_entries) {
        string_list_free(&files);
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot allocate tree payload");
        return GIT_RESULT_IO_ERROR;
    }

    pushed_count = files.count;

    if (oid_is_zero(remote_head)) {
        result = github_bootstrap_empty_repo_push(archive,
            repo_root,
            &files,
            owner,
            repo,
            token,
            resolved_branch,
            commit_message,
            new_commit_oid,
            sizeof(new_commit_oid),
            out_message,
            out_message_len);

        json_decref(tree_entries);
        tree_entries = NULL;
        string_list_free(&files);

        if (result != GIT_RESULT_OK) {
            close_sdmc_archive(archive);
            return result;
        }

        result = save_remote_state(archive, repo_root, resolved_url, resolved_branch, new_commit_oid);
        if (result == GIT_RESULT_OK)
            result = write_git_remote_config(archive, repo_root, resolved_url, resolved_branch);

        close_sdmc_archive(archive);

        if (result != GIT_RESULT_OK) {
            set_message(out_message, out_message_len, "Push succeeded but local remote state update failed");
            return result;
        }

        snprintf(out_branch, out_branch_len, "%s", resolved_branch);
        snprintf(out_head_oid, out_head_oid_len, "%s", new_commit_oid);

        {
            char short_sha[8];
            char message[176];
            memcpy(short_sha, new_commit_oid, 7);
            short_sha[7] = '\0';
            snprintf(message, sizeof(message), "Push completed: %lu file(s), %s", (unsigned long)pushed_count, short_sha);
            set_message(out_message, out_message_len, message);
        }

        return GIT_RESULT_OK;
    }

    for (i = 0; i < files.count; i++) {
        const char *rel_path = files.items[i];
        char abs_path[GIT_PATH_MAX];
        unsigned char *file_data = NULL;
        size_t file_size = 0;
        char blob_oid[GIT_OID_HEX_LEN + 1];
        json_t *entry_obj;

        if (!join_path(repo_root, rel_path, abs_path, sizeof(abs_path))) {
            json_decref(tree_entries);
            string_list_free(&files);
            close_sdmc_archive(archive);
            set_message(out_message, out_message_len, "File path too long");
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }

        result = read_file_alloc(archive, abs_path, &file_data, &file_size);
        if (result != GIT_RESULT_OK) {
            json_decref(tree_entries);
            string_list_free(&files);
            close_sdmc_archive(archive);
            set_message(out_message, out_message_len, "Cannot read local file for push");
            return result;
        }

        result = github_create_blob(owner, repo, token, file_data, file_size,
            blob_oid, sizeof(blob_oid), out_message, out_message_len);
        free(file_data);
        if (result != GIT_RESULT_OK) {
            json_decref(tree_entries);
            string_list_free(&files);
            close_sdmc_archive(archive);
            return result;
        }

        entry_obj = json_object();
        if (!entry_obj) {
            json_decref(tree_entries);
            string_list_free(&files);
            close_sdmc_archive(archive);
            set_message(out_message, out_message_len, "Cannot allocate tree entry");
            return GIT_RESULT_IO_ERROR;
        }

        json_object_set_new(entry_obj, "path", json_string(rel_path));
        json_object_set_new(entry_obj, "mode", json_string("100644"));
        json_object_set_new(entry_obj, "type", json_string("blob"));
        json_object_set_new(entry_obj, "sha", json_string(blob_oid));
        json_array_append_new(tree_entries, entry_obj);
    }

    result = github_create_tree(owner, repo, token, tree_entries, new_tree_oid, sizeof(new_tree_oid), out_message, out_message_len);
    if (result != GIT_RESULT_OK) {
        json_decref(tree_entries);
        string_list_free(&files);
        close_sdmc_archive(archive);
        return result;
    }

    result = github_create_commit(owner, repo, token, commit_message, new_tree_oid,
        oid_is_zero(remote_head) ? NULL : remote_head,
        new_commit_oid, sizeof(new_commit_oid),
        out_message, out_message_len);
    if (result != GIT_RESULT_OK) {
        json_decref(tree_entries);
        string_list_free(&files);
        close_sdmc_archive(archive);
        return result;
    }

    result = github_update_or_create_ref(owner, repo, resolved_branch, token, new_commit_oid,
        out_message, out_message_len);
    if (result != GIT_RESULT_OK) {
        json_decref(tree_entries);
        string_list_free(&files);
        close_sdmc_archive(archive);
        return result;
    }

    result = save_remote_state(archive, repo_root, resolved_url, resolved_branch, new_commit_oid);
    if (result == GIT_RESULT_OK)
        result = write_git_remote_config(archive, repo_root, resolved_url, resolved_branch);

    json_decref(tree_entries);
    string_list_free(&files);
    close_sdmc_archive(archive);

    if (result != GIT_RESULT_OK) {
        set_message(out_message, out_message_len, "Push succeeded but local remote state update failed");
        return result;
    }

    snprintf(out_branch, out_branch_len, "%s", resolved_branch);
    snprintf(out_head_oid, out_head_oid_len, "%s", new_commit_oid);

    {
        char short_sha[8];
        char message[176];
        memcpy(short_sha, new_commit_oid, 7);
        short_sha[7] = '\0';
        snprintf(message, sizeof(message), "Push completed: %lu file(s), %s", (unsigned long)pushed_count, short_sha);
        set_message(out_message, out_message_len, message);
    }

    return GIT_RESULT_OK;
}