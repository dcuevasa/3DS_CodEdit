#include <3ds.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "git/git.h"

#define GIT_MAX_PATH 1024

static void set_message(char *out_message, size_t out_message_len, const char *message) {
    if (!out_message || out_message_len == 0)
        return;

    if (!message)
        message = "";

    snprintf(out_message, out_message_len, "%s", message);
}

static bool normalize_dir_path(const char *path, char *out_path, size_t out_path_len) {
    size_t length;

    if (!path || !out_path || out_path_len < 2)
        return false;

    length = strnlen(path, out_path_len - 1);
    if (length == 0)
        return false;

    if (length >= out_path_len)
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

static bool join_path(const char *base, const char *leaf, char *out_path, size_t out_path_len) {
    int written;

    if (!base || !leaf || !out_path || out_path_len < 2)
        return false;

    if (leaf[0] == '/')
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

static GitResult write_text_file(FS_Archive archive, const char *path, const char *text, bool overwrite) {
    Handle file = 0;
    Result ret;
    u64 size = 0;
    u32 bytes_written = 0;

    if (!path)
        return GIT_RESULT_INVALID_ARG;

    if (!text)
        text = "";

    size = (u64)strlen(text);

    if (overwrite)
        FSUSER_DeleteFile(archive, fsMakePath(PATH_ASCII, path));

    if (!file_exists(archive, path)) {
        ret = FSUSER_CreateFile(archive, fsMakePath(PATH_ASCII, path), 0, size);
        if (R_FAILED(ret))
            return GIT_RESULT_IO_ERROR;
    }

    if (R_FAILED(FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, path), FS_OPEN_WRITE, 0)))
        return GIT_RESULT_IO_ERROR;

    if (R_FAILED(FSFILE_SetSize(file, size))) {
        FSFILE_Close(file);
        return GIT_RESULT_IO_ERROR;
    }

    if (size > 0) {
        ret = FSFILE_Write(file, &bytes_written, 0, text, (u32)size, FS_WRITE_FLUSH);
        if (R_FAILED(ret) || bytes_written != (u32)size) {
            FSFILE_Close(file);
            return GIT_RESULT_IO_ERROR;
        }
    }

    FSFILE_Close(file);
    return GIT_RESULT_OK;
}

static GitResult read_text_file(FS_Archive archive, const char *path, char *out_text, size_t out_text_len) {
    Handle file = 0;
    Result ret;
    u64 size = 0;
    u32 bytes_read = 0;
    u32 to_read;

    if (!path || !out_text || out_text_len == 0)
        return GIT_RESULT_INVALID_ARG;

    out_text[0] = '\0';

    if (R_FAILED(FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0)))
        return GIT_RESULT_NOT_FOUND;

    if (R_FAILED(FSFILE_GetSize(file, &size))) {
        FSFILE_Close(file);
        return GIT_RESULT_IO_ERROR;
    }

    to_read = (u32)((size < (u64)(out_text_len - 1)) ? size : (u64)(out_text_len - 1));
    if (to_read > 0) {
        ret = FSFILE_Read(file, &bytes_read, 0, out_text, to_read);
        if (R_FAILED(ret)) {
            FSFILE_Close(file);
            out_text[0] = '\0';
            return GIT_RESULT_IO_ERROR;
        }
    }

    FSFILE_Close(file);
    out_text[bytes_read] = '\0';

    if (size >= (u64)out_text_len)
        return GIT_RESULT_BUFFER_TOO_SMALL;

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

GitResult git_init_repository(const char *worktree_path, char *out_message, size_t out_message_len) {
    FS_Archive archive;
    char root[GIT_MAX_PATH];
    char git_path[GIT_MAX_PATH];
    char objects_path[GIT_MAX_PATH];
    char refs_path[GIT_MAX_PATH];
    char refs_heads_path[GIT_MAX_PATH];
    char head_file[GIT_MAX_PATH];
    char config_file[GIT_MAX_PATH];
    char description_file[GIT_MAX_PATH];
    const char *head_content = "ref: refs/heads/main\n";
    const char *config_content = "[core]\n\trepositoryformatversion = 0\n\tfilemode = true\n\tbare = false\n\tlogallrefupdates = true\n";
    const char *description_content = "Unnamed repository; edit this file to name the repository.\n";
    GitResult git_result;

    if (!normalize_dir_path(worktree_path, root, sizeof(root))) {
        set_message(out_message, out_message_len, "Invalid project path");
        return GIT_RESULT_INVALID_ARG;
    }

    if (!open_sdmc_archive(&archive)) {
        set_message(out_message, out_message_len, "Failed to open SD archive");
        return GIT_RESULT_IO_ERROR;
    }

    if (!directory_exists(archive, root)) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Project folder not found");
        return GIT_RESULT_NOT_FOUND;
    }

    if (!join_path(root, ".git", git_path, sizeof(git_path))) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Path too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    if (directory_exists(archive, git_path)) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Repository already exists");
        return GIT_RESULT_ALREADY_EXISTS;
    }

    if (!join_path(git_path, "objects", objects_path, sizeof(objects_path)) ||
        !join_path(git_path, "refs", refs_path, sizeof(refs_path)) ||
        !join_path(refs_path, "heads", refs_heads_path, sizeof(refs_heads_path)) ||
        !join_path(git_path, "HEAD", head_file, sizeof(head_file)) ||
        !join_path(git_path, "config", config_file, sizeof(config_file)) ||
        !join_path(git_path, "description", description_file, sizeof(description_file))) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Path too long");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    git_result = ensure_directory(archive, git_path);
    if (git_result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot create .git directory");
        return git_result;
    }

    git_result = ensure_directory(archive, objects_path);
    if (git_result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot create objects directory");
        return git_result;
    }

    git_result = ensure_directory(archive, refs_path);
    if (git_result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot create refs directory");
        return git_result;
    }

    git_result = ensure_directory(archive, refs_heads_path);
    if (git_result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot create refs/heads directory");
        return git_result;
    }

    git_result = write_text_file(archive, head_file, head_content, true);
    if (git_result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot write HEAD");
        return git_result;
    }

    git_result = write_text_file(archive, config_file, config_content, true);
    if (git_result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot write config");
        return git_result;
    }

    git_result = write_text_file(archive, description_file, description_content, true);
    if (git_result != GIT_RESULT_OK) {
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot write description");
        return git_result;
    }

    close_sdmc_archive(archive);
    set_message(out_message, out_message_len, "Git repository initialized (.git)");
    return GIT_RESULT_OK;
}

bool git_find_repository_root(const char *start_path, char *out_repo_root, size_t out_repo_root_len) {
    FS_Archive archive;
    char current_path[GIT_MAX_PATH];
    char git_path[GIT_MAX_PATH];

    if (!out_repo_root || out_repo_root_len == 0)
        return false;

    out_repo_root[0] = '\0';

    if (!normalize_dir_path(start_path, current_path, sizeof(current_path)))
        return false;

    if (!open_sdmc_archive(&archive))
        return false;

    while (true) {
        if (!join_path(current_path, ".git", git_path, sizeof(git_path)))
            break;

        if (directory_exists(archive, git_path)) {
            snprintf(out_repo_root, out_repo_root_len, "%s", current_path);
            close_sdmc_archive(archive);
            return true;
        }

        if (strcmp(current_path, "/") == 0)
            break;

        parent_dir(current_path);
    }

    close_sdmc_archive(archive);
    return false;
}

GitResult git_get_current_branch(const char *repo_root, char *out_branch, size_t out_branch_len) {
    FS_Archive archive;
    char root[GIT_MAX_PATH];
    char head_path[GIT_MAX_PATH];
    char head_text[256];
    const char *prefix = "ref: refs/heads/";
    const char *ref = NULL;
    const char *branch_start = NULL;
    const char *last_slash = NULL;

    if (!out_branch || out_branch_len == 0)
        return GIT_RESULT_INVALID_ARG;

    out_branch[0] = '\0';

    if (!normalize_dir_path(repo_root, root, sizeof(root)))
        return GIT_RESULT_INVALID_ARG;

    if (!join_path(root, ".git/HEAD", head_path, sizeof(head_path)))
        return GIT_RESULT_BUFFER_TOO_SMALL;

    if (!open_sdmc_archive(&archive))
        return GIT_RESULT_IO_ERROR;

    if (read_text_file(archive, head_path, head_text, sizeof(head_text)) < 0) {
        close_sdmc_archive(archive);
        return GIT_RESULT_NOT_FOUND;
    }

    close_sdmc_archive(archive);

    trim_line_end(head_text);

    if (strncmp(head_text, prefix, strlen(prefix)) == 0) {
        branch_start = head_text + strlen(prefix);
    }
    else if (strncmp(head_text, "ref: ", 5) == 0) {
        ref = head_text + 5;
        last_slash = strrchr(ref, '/');
        branch_start = last_slash ? (last_slash + 1) : ref;
    }
    else {
        snprintf(out_branch, out_branch_len, "detached");
        return GIT_RESULT_OK;
    }

    if (!branch_start || branch_start[0] == '\0') {
        snprintf(out_branch, out_branch_len, "unknown");
        return GIT_RESULT_OK;
    }

    snprintf(out_branch, out_branch_len, "%s", branch_start);
    return GIT_RESULT_OK;
}
