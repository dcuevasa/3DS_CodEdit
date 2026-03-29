#ifndef _3DS_CODEDIT_GIT_H
#define _3DS_CODEDIT_GIT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GitResult {
    GIT_RESULT_OK = 0,
    GIT_RESULT_INVALID_ARG = -1,
    GIT_RESULT_IO_ERROR = -2,
    GIT_RESULT_ALREADY_EXISTS = -3,
    GIT_RESULT_NOT_FOUND = -4,
    GIT_RESULT_BUFFER_TOO_SMALL = -5,
    GIT_RESULT_PARSE_ERROR = -6,
    GIT_RESULT_NO_REPOSITORY = -7,
    GIT_RESULT_NO_CHANGES = -8
} GitResult;

#define GIT_PATH_MAX 1024
#define GIT_OID_HEX_LEN 40

GitResult git_init_repository(const char *worktree_path, char *out_message, size_t out_message_len);
bool git_find_repository_root(const char *start_path, char *out_repo_root, size_t out_repo_root_len);
GitResult git_get_current_branch(const char *repo_root, char *out_branch, size_t out_branch_len);
GitResult git_get_staged_count(const char *start_path, int *out_staged_count);
GitResult git_add_all(const char *start_path, char *out_message, size_t out_message_len);
GitResult git_commit(const char *start_path, const char *message, char *out_commit_oid, size_t out_commit_oid_len,
    char *out_message, size_t out_message_len);
GitResult git_remote_probe_github(const char *remote_url, const char *token,
    char *out_default_branch, size_t out_default_branch_len,
    char *out_head_oid, size_t out_head_oid_len,
    char *out_message, size_t out_message_len);
GitResult git_remote_clone_github(const char *target_path, const char *remote_url, const char *branch_hint, const char *token,
    char *out_default_branch, size_t out_default_branch_len,
    char *out_head_oid, size_t out_head_oid_len,
    char *out_message, size_t out_message_len);
GitResult git_remote_fetch_github(const char *start_path, const char *remote_url, const char *branch_hint, const char *token,
    bool *out_has_updates,
    char *out_branch, size_t out_branch_len,
    char *out_head_oid, size_t out_head_oid_len,
    char *out_message, size_t out_message_len);
GitResult git_remote_pull_github(const char *start_path, const char *remote_url, const char *branch_hint, const char *token,
    char *out_branch, size_t out_branch_len,
    char *out_head_oid, size_t out_head_oid_len,
    char *out_message, size_t out_message_len);
GitResult git_remote_push_github(const char *start_path, const char *remote_url, const char *branch_hint,
    const char *token, const char *commit_message,
    char *out_branch, size_t out_branch_len,
    char *out_head_oid, size_t out_head_oid_len,
    char *out_message, size_t out_message_len);

#ifdef __cplusplus
}
#endif

#endif
