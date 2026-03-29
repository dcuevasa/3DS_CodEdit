#include <3ds.h>

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zlib.h>

#include "git/git.h"

#define GIT_INDEX_FILE ".git/index.cedit"
#define GIT_MAX_TREE_DEPTH 24
#define GIT_DEFAULT_FILE_MODE 0100644

#define GIT_SHA1_BLOCK_SIZE 64
#define GIT_SHA1_DIGEST_SIZE 20

typedef struct {
    uint32_t state[5];
    uint64_t bit_count;
    uint8_t buffer[GIT_SHA1_BLOCK_SIZE];
} Sha1Context;

typedef struct {
    char path[GIT_PATH_MAX];
    unsigned char oid[GIT_SHA1_DIGEST_SIZE];
    char oid_hex[GIT_OID_HEX_LEN + 1];
    uint32_t mode;
} StageEntry;

typedef struct {
    StageEntry *items;
    size_t count;
    size_t capacity;
} StageList;

typedef struct {
    char name[256];
    unsigned char oid[GIT_SHA1_DIGEST_SIZE];
    uint32_t mode;
} TreeEntry;

typedef struct {
    TreeEntry *items;
    size_t count;
    size_t capacity;
} TreeList;

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

static uint32_t rol32(uint32_t value, uint32_t bits) {
    return (value << bits) | (value >> (32U - bits));
}

static void sha1_transform(Sha1Context *ctx, const uint8_t block[GIT_SHA1_BLOCK_SIZE]) {
    uint32_t w[80];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t k;
    uint32_t temp;
    size_t i;

    for (i = 0; i < 16; i++) {
        size_t off = i * 4;
        w[i] = ((uint32_t)block[off] << 24) |
               ((uint32_t)block[off + 1] << 16) |
               ((uint32_t)block[off + 2] << 8) |
               (uint32_t)block[off + 3];
    }

    for (i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];

    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        }
        else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        }
        else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        }
        else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        temp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = temp;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

static void sha1_init(Sha1Context *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->bit_count = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

static void sha1_update(Sha1Context *ctx, const uint8_t *data, size_t len) {
    size_t used;

    if (!data || len == 0)
        return;

    used = (size_t)((ctx->bit_count >> 3) & 0x3F);
    ctx->bit_count += (uint64_t)len * 8ULL;

    if (used > 0) {
        size_t to_fill = GIT_SHA1_BLOCK_SIZE - used;
        if (len < to_fill) {
            memcpy(ctx->buffer + used, data, len);
            return;
        }

        memcpy(ctx->buffer + used, data, to_fill);
        sha1_transform(ctx, ctx->buffer);
        data += to_fill;
        len -= to_fill;
    }

    while (len >= GIT_SHA1_BLOCK_SIZE) {
        sha1_transform(ctx, data);
        data += GIT_SHA1_BLOCK_SIZE;
        len -= GIT_SHA1_BLOCK_SIZE;
    }

    if (len > 0)
        memcpy(ctx->buffer, data, len);
}

static void sha1_final(Sha1Context *ctx, uint8_t digest[GIT_SHA1_DIGEST_SIZE]) {
    uint8_t padding[GIT_SHA1_BLOCK_SIZE] = { 0x80 };
    uint8_t bit_length[8];
    uint64_t bits = ctx->bit_count;
    size_t i;
    size_t used;
    size_t pad_len;

    for (i = 0; i < 8; i++) {
        bit_length[7 - i] = (uint8_t)(bits & 0xFFU);
        bits >>= 8;
    }

    used = (size_t)((ctx->bit_count >> 3) & 0x3F);
    pad_len = (used < 56) ? (56 - used) : (120 - used);

    sha1_update(ctx, padding, pad_len);
    sha1_update(ctx, bit_length, 8);

    for (i = 0; i < 5; i++) {
        digest[i * 4] = (uint8_t)((ctx->state[i] >> 24) & 0xFFU);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFFU);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xFFU);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xFFU);
    }
}

static void oid_to_hex(const unsigned char oid[GIT_SHA1_DIGEST_SIZE], char out_hex[GIT_OID_HEX_LEN + 1]) {
    static const char *hex = "0123456789abcdef";
    size_t i;

    for (i = 0; i < GIT_SHA1_DIGEST_SIZE; i++) {
        out_hex[i * 2] = hex[(oid[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = hex[oid[i] & 0x0F];
    }

    out_hex[GIT_OID_HEX_LEN] = '\0';
}

static int hex_to_nibble(char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static bool oid_from_hex(const char *hex, unsigned char out_oid[GIT_SHA1_DIGEST_SIZE]) {
    size_t i;

    if (!hex || strlen(hex) != GIT_OID_HEX_LEN)
        return false;

    for (i = 0; i < GIT_SHA1_DIGEST_SIZE; i++) {
        int hi = hex_to_nibble(hex[i * 2]);
        int lo = hex_to_nibble(hex[i * 2 + 1]);

        if (hi < 0 || lo < 0)
            return false;

        out_oid[i] = (unsigned char)((hi << 4) | lo);
    }

    return true;
}

static bool oid_hex_is_valid(const char *hex) {
    size_t i;

    if (!hex || strlen(hex) != GIT_OID_HEX_LEN)
        return false;

    for (i = 0; i < GIT_OID_HEX_LEN; i++) {
        if (!isxdigit((unsigned char)hex[i]))
            return false;
    }

    return true;
}

static GitResult write_loose_object(FS_Archive archive, const char *repo_root, const char *type,
    const unsigned char *data, size_t data_len, unsigned char out_oid[GIT_SHA1_DIGEST_SIZE],
    char out_oid_hex[GIT_OID_HEX_LEN + 1]) {
    char header[64];
    size_t header_len;
    size_t payload_len;
    unsigned char *payload = NULL;
    unsigned char oid[GIT_SHA1_DIGEST_SIZE];
    char oid_hex[GIT_OID_HEX_LEN + 1];
    Sha1Context sha;
    char objects_root[GIT_PATH_MAX];
    char object_dir[GIT_PATH_MAX];
    char object_path[GIT_PATH_MAX];
    char fanout[3];
    uLongf compressed_len;
    unsigned char *compressed = NULL;
    GitResult result = GIT_RESULT_OK;

    if (!repo_root || !type)
        return GIT_RESULT_INVALID_ARG;

    header_len = (size_t)snprintf(header, sizeof(header), "%s %lu", type, (unsigned long)data_len);
    if (header_len + 1 >= sizeof(header))
        return GIT_RESULT_BUFFER_TOO_SMALL;

    payload_len = header_len + 1 + data_len;
    payload = (unsigned char *)malloc((payload_len > 0) ? payload_len : 1);
    if (!payload)
        return GIT_RESULT_IO_ERROR;

    memcpy(payload, header, header_len);
    payload[header_len] = '\0';
    if (data_len > 0 && data)
        memcpy(payload + header_len + 1, data, data_len);

    sha1_init(&sha);
    sha1_update(&sha, payload, payload_len);
    sha1_final(&sha, oid);
    oid_to_hex(oid, oid_hex);

    if (!join_path(repo_root, ".git/objects", objects_root, sizeof(objects_root))) {
        free(payload);
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    fanout[0] = oid_hex[0];
    fanout[1] = oid_hex[1];
    fanout[2] = '\0';

    if (!join_path(objects_root, fanout, object_dir, sizeof(object_dir)) ||
        !join_path(object_dir, oid_hex + 2, object_path, sizeof(object_path))) {
        free(payload);
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    result = ensure_directory_recursive(archive, object_dir);
    if (result != GIT_RESULT_OK) {
        free(payload);
        return result;
    }

    if (file_exists(archive, object_path)) {
        free(payload);
        if (out_oid)
            memcpy(out_oid, oid, sizeof(oid));
        if (out_oid_hex)
            snprintf(out_oid_hex, GIT_OID_HEX_LEN + 1, "%s", oid_hex);
        return GIT_RESULT_OK;
    }

    compressed_len = compressBound((uLong)payload_len);
    compressed = (unsigned char *)malloc(compressed_len);
    if (!compressed) {
        free(payload);
        return GIT_RESULT_IO_ERROR;
    }

    if (compress2(compressed, &compressed_len, payload, (uLong)payload_len, Z_BEST_SPEED) != Z_OK) {
        free(compressed);
        free(payload);
        return GIT_RESULT_IO_ERROR;
    }

    result = write_file_bytes(archive, object_path, compressed, compressed_len, false);

    free(compressed);
    free(payload);

    if (result != GIT_RESULT_OK)
        return result;

    if (out_oid)
        memcpy(out_oid, oid, sizeof(oid));
    if (out_oid_hex)
        snprintf(out_oid_hex, GIT_OID_HEX_LEN + 1, "%s", oid_hex);

    return GIT_RESULT_OK;
}

static void stage_list_init(StageList *list) {
    if (!list)
        return;

    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void stage_list_free(StageList *list) {
    if (!list)
        return;

    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static GitResult stage_list_push(StageList *list, const StageEntry *entry) {
    size_t new_capacity;
    StageEntry *new_items;

    if (!list || !entry)
        return GIT_RESULT_INVALID_ARG;

    if (list->count == list->capacity) {
        new_capacity = (list->capacity == 0) ? 64 : (list->capacity * 2);
        new_items = (StageEntry *)realloc(list->items, new_capacity * sizeof(StageEntry));
        if (!new_items)
            return GIT_RESULT_IO_ERROR;

        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count] = *entry;
    list->count++;

    return GIT_RESULT_OK;
}

static int stage_entry_compare(const void *lhs, const void *rhs) {
    const StageEntry *a = (const StageEntry *)lhs;
    const StageEntry *b = (const StageEntry *)rhs;

    return strcmp(a->path, b->path);
}

static void stage_list_sort(StageList *list) {
    if (!list || list->count <= 1)
        return;

    qsort(list->items, list->count, sizeof(StageEntry), stage_entry_compare);
}

static bool entry_name_to_ascii(const FS_DirectoryEntry *entry, char *out_name, size_t out_name_len) {
    size_t i = 0;

    if (!entry || !out_name || out_name_len == 0)
        return false;

    while (i + 1 < out_name_len) {
        u16 ch = entry->name[i];
        if (ch == 0)
            break;

        out_name[i] = (ch < 0x80U) ? (char)ch : '_';
        i++;
    }

    if (i + 1 >= out_name_len && entry->name[i] != 0)
        return false;

    out_name[i] = '\0';
    return true;
}

static GitResult add_file_to_stage(FS_Archive archive, const char *repo_root, const char *abs_path, const char *rel_path,
    StageList *staged) {
    unsigned char *file_data = NULL;
    size_t file_size = 0;
    StageEntry entry;
    GitResult result;

    result = read_file_alloc(archive, abs_path, &file_data, &file_size);
    if (result != GIT_RESULT_OK)
        return result;

    memset(&entry, 0, sizeof(entry));
    entry.mode = GIT_DEFAULT_FILE_MODE;

    result = write_loose_object(archive, repo_root, "blob", file_data, file_size, entry.oid, entry.oid_hex);
    free(file_data);

    if (result != GIT_RESULT_OK)
        return result;

    if (snprintf(entry.path, sizeof(entry.path), "%s", rel_path) <= 0 || strlen(entry.path) >= sizeof(entry.path))
        return GIT_RESULT_BUFFER_TOO_SMALL;

    return stage_list_push(staged, &entry);
}

static GitResult scan_directory_for_add(FS_Archive archive, const char *repo_root, const char *current_abs,
    const char *current_rel, int depth, StageList *staged) {
    Handle dir = 0;
    u32 entry_count = 0;
    GitResult result = GIT_RESULT_OK;

    if (!repo_root || !current_abs || !staged)
        return GIT_RESULT_INVALID_ARG;

    if (depth > GIT_MAX_TREE_DEPTH)
        return GIT_RESULT_IO_ERROR;

    if (R_FAILED(FSUSER_OpenDirectory(&dir, archive, fsMakePath(PATH_ASCII, current_abs))))
        return GIT_RESULT_IO_ERROR;

    do {
        FS_DirectoryEntry entry;
        char entry_name[256];
        char child_abs[GIT_PATH_MAX];
        char child_rel[GIT_PATH_MAX];
        Result fs_result = FSDIR_Read(dir, &entry_count, 1, &entry);

        if (R_FAILED(fs_result)) {
            result = GIT_RESULT_IO_ERROR;
            break;
        }

        if (entry_count != 1)
            break;

        if (!entry_name_to_ascii(&entry, entry_name, sizeof(entry_name))) {
            result = GIT_RESULT_BUFFER_TOO_SMALL;
            break;
        }

        if (entry_name[0] == '\0' || strcmp(entry_name, ".") == 0 || strcmp(entry_name, "..") == 0)
            continue;

        if (!join_path(current_abs, entry_name, child_abs, sizeof(child_abs))) {
            result = GIT_RESULT_BUFFER_TOO_SMALL;
            break;
        }

        if (current_rel && current_rel[0] != '\0') {
            int written = snprintf(child_rel, sizeof(child_rel), "%s/%s", current_rel, entry_name);
            if (written <= 0 || (size_t)written >= sizeof(child_rel)) {
                result = GIT_RESULT_BUFFER_TOO_SMALL;
                break;
            }
        }
        else {
            int written = snprintf(child_rel, sizeof(child_rel), "%s", entry_name);
            if (written <= 0 || (size_t)written >= sizeof(child_rel)) {
                result = GIT_RESULT_BUFFER_TOO_SMALL;
                break;
            }
        }

        if (entry.attributes & FS_ATTRIBUTE_DIRECTORY) {
            if (strcmp(entry_name, ".git") == 0)
                continue;

            result = scan_directory_for_add(archive, repo_root, child_abs, child_rel, depth + 1, staged);
            if (result != GIT_RESULT_OK)
                break;
        }
        else {
            result = add_file_to_stage(archive, repo_root, child_abs, child_rel, staged);
            if (result != GIT_RESULT_OK)
                break;
        }
    } while (entry_count > 0);

    FSDIR_Close(dir);
    return result;
}

static GitResult write_stage_index(FS_Archive archive, const char *repo_root, const StageList *staged) {
    char index_path[GIT_PATH_MAX];
    char *text = NULL;
    size_t total = 0;
    size_t i;
    size_t offset = 0;
    GitResult result;

    if (!repo_root || !staged)
        return GIT_RESULT_INVALID_ARG;

    if (!join_path(repo_root, GIT_INDEX_FILE, index_path, sizeof(index_path)))
        return GIT_RESULT_BUFFER_TOO_SMALL;

    if (staged->count == 0)
        return write_file_bytes(archive, index_path, (const unsigned char *)"", 0, true);

    for (i = 0; i < staged->count; i++) {
        total += 8 + GIT_OID_HEX_LEN + 1 + strlen(staged->items[i].path) + 1;
    }

    text = (char *)malloc(total + 1);
    if (!text)
        return GIT_RESULT_IO_ERROR;

    for (i = 0; i < staged->count; i++) {
        int written = snprintf(text + offset, (total + 1) - offset, "%o %s %s\n",
            (unsigned int)staged->items[i].mode, staged->items[i].oid_hex, staged->items[i].path);

        if (written <= 0 || (size_t)written >= ((total + 1) - offset)) {
            free(text);
            return GIT_RESULT_IO_ERROR;
        }

        offset += (size_t)written;
    }

    result = write_file_bytes(archive, index_path, (const unsigned char *)text, offset, true);
    free(text);
    return result;
}

static GitResult parse_stage_line(char *line, StageEntry *out_entry) {
    char *first_space;
    char *second_space;
    char *path_start;
    char *endptr = NULL;
    unsigned long mode;

    if (!line || !out_entry)
        return GIT_RESULT_INVALID_ARG;

    trim_line_end(line);
    if (line[0] == '\0')
        return GIT_RESULT_NOT_FOUND;

    first_space = strchr(line, ' ');
    if (!first_space)
        return GIT_RESULT_PARSE_ERROR;

    *first_space = '\0';
    second_space = first_space + 1;
    while (*second_space == ' ')
        second_space++;

    path_start = strchr(second_space, ' ');
    if (!path_start)
        return GIT_RESULT_PARSE_ERROR;

    *path_start = '\0';
    path_start++;
    while (*path_start == ' ')
        path_start++;

    trim_line_end(path_start);
    if (path_start[0] == '\0')
        return GIT_RESULT_PARSE_ERROR;

    if (!oid_hex_is_valid(second_space))
        return GIT_RESULT_PARSE_ERROR;

    mode = strtoul(line, &endptr, 8);
    if (endptr == line || mode == 0)
        mode = GIT_DEFAULT_FILE_MODE;

    memset(out_entry, 0, sizeof(*out_entry));
    out_entry->mode = (uint32_t)mode;

    snprintf(out_entry->oid_hex, sizeof(out_entry->oid_hex), "%s", second_space);
    if (snprintf(out_entry->path, sizeof(out_entry->path), "%s", path_start) <= 0 ||
        strlen(out_entry->path) >= sizeof(out_entry->path)) {
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    if (!oid_from_hex(out_entry->oid_hex, out_entry->oid))
        return GIT_RESULT_PARSE_ERROR;

    return GIT_RESULT_OK;
}

static GitResult load_stage_index(FS_Archive archive, const char *repo_root, StageList *out_staged) {
    char index_path[GIT_PATH_MAX];
    unsigned char *data = NULL;
    size_t size = 0;
    GitResult result;
    char *cursor;
    char *end;

    if (!repo_root || !out_staged)
        return GIT_RESULT_INVALID_ARG;

    if (!join_path(repo_root, GIT_INDEX_FILE, index_path, sizeof(index_path)))
        return GIT_RESULT_BUFFER_TOO_SMALL;

    if (!file_exists(archive, index_path))
        return GIT_RESULT_NO_CHANGES;

    result = read_file_alloc(archive, index_path, &data, &size);
    if (result != GIT_RESULT_OK)
        return result;

    if (size == 0) {
        free(data);
        return GIT_RESULT_NO_CHANGES;
    }

    cursor = (char *)data;
    end = (char *)data + size;

    while (cursor < end) {
        char *line = cursor;
        StageEntry entry;

        while (cursor < end && *cursor != '\n')
            cursor++;

        if (cursor < end) {
            *cursor = '\0';
            cursor++;
        }
        else {
            *cursor = '\0';
        }

        result = parse_stage_line(line, &entry);
        if (result == GIT_RESULT_NOT_FOUND)
            continue;

        if (result != GIT_RESULT_OK) {
            free(data);
            return result;
        }

        result = stage_list_push(out_staged, &entry);
        if (result != GIT_RESULT_OK) {
            free(data);
            return result;
        }
    }

    free(data);

    if (out_staged->count == 0)
        return GIT_RESULT_NO_CHANGES;

    stage_list_sort(out_staged);
    return GIT_RESULT_OK;
}

static void tree_list_init(TreeList *list) {
    if (!list)
        return;

    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void tree_list_free(TreeList *list) {
    if (!list)
        return;

    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static GitResult tree_list_push(TreeList *list, const TreeEntry *entry) {
    size_t new_capacity;
    TreeEntry *new_items;

    if (!list || !entry)
        return GIT_RESULT_INVALID_ARG;

    if (list->count == list->capacity) {
        new_capacity = (list->capacity == 0) ? 32 : (list->capacity * 2);
        new_items = (TreeEntry *)realloc(list->items, new_capacity * sizeof(TreeEntry));
        if (!new_items)
            return GIT_RESULT_IO_ERROR;

        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count] = *entry;
    list->count++;

    return GIT_RESULT_OK;
}

static int tree_list_find(const TreeList *list, const char *name) {
    size_t i;

    if (!list || !name)
        return -1;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].name, name) == 0)
            return (int)i;
    }

    return -1;
}

static int tree_entry_compare(const void *lhs, const void *rhs) {
    const TreeEntry *a = (const TreeEntry *)lhs;
    const TreeEntry *b = (const TreeEntry *)rhs;

    return strcmp(a->name, b->name);
}

static void tree_list_sort(TreeList *list) {
    if (!list || list->count <= 1)
        return;

    qsort(list->items, list->count, sizeof(TreeEntry), tree_entry_compare);
}

static GitResult build_tree_for_prefix(FS_Archive archive, const char *repo_root, const StageList *staged,
    const char *prefix, int depth, unsigned char out_tree_oid[GIT_SHA1_DIGEST_SIZE],
    char out_tree_oid_hex[GIT_OID_HEX_LEN + 1]) {
    size_t i;
    size_t prefix_len;
    TreeList children;
    GitResult result = GIT_RESULT_OK;
    unsigned char *tree_data = NULL;
    size_t tree_size = 0;
    size_t offset = 0;

    if (!repo_root || !staged || !prefix)
        return GIT_RESULT_INVALID_ARG;

    if (depth > GIT_MAX_TREE_DEPTH)
        return GIT_RESULT_IO_ERROR;

    prefix_len = strlen(prefix);
    tree_list_init(&children);

    for (i = 0; i < staged->count; i++) {
        const StageEntry *entry = &staged->items[i];
        const char *remainder;
        const char *slash;

        if (strncmp(entry->path, prefix, prefix_len) != 0)
            continue;

        remainder = entry->path + prefix_len;
        if (remainder[0] == '\0')
            continue;

        slash = strchr(remainder, '/');

        if (!slash) {
            TreeEntry tree_entry;

            if (tree_list_find(&children, remainder) >= 0)
                continue;

            memset(&tree_entry, 0, sizeof(tree_entry));
            if (snprintf(tree_entry.name, sizeof(tree_entry.name), "%s", remainder) <= 0 ||
                strlen(tree_entry.name) >= sizeof(tree_entry.name)) {
                result = GIT_RESULT_BUFFER_TOO_SMALL;
                goto cleanup;
            }

            tree_entry.mode = (entry->mode == 0) ? GIT_DEFAULT_FILE_MODE : entry->mode;
            memcpy(tree_entry.oid, entry->oid, sizeof(tree_entry.oid));

            result = tree_list_push(&children, &tree_entry);
            if (result != GIT_RESULT_OK)
                goto cleanup;
        }
        else {
            size_t dir_len = (size_t)(slash - remainder);
            char dir_name[256];
            char child_prefix[GIT_PATH_MAX];
            TreeEntry tree_entry;

            if (dir_len == 0 || dir_len >= sizeof(dir_name)) {
                result = GIT_RESULT_BUFFER_TOO_SMALL;
                goto cleanup;
            }

            memcpy(dir_name, remainder, dir_len);
            dir_name[dir_len] = '\0';

            if (tree_list_find(&children, dir_name) >= 0)
                continue;

            if (snprintf(child_prefix, sizeof(child_prefix), "%s%.*s/", prefix, (int)dir_len, remainder) <= 0 ||
                strlen(child_prefix) >= sizeof(child_prefix)) {
                result = GIT_RESULT_BUFFER_TOO_SMALL;
                goto cleanup;
            }

            memset(&tree_entry, 0, sizeof(tree_entry));
            snprintf(tree_entry.name, sizeof(tree_entry.name), "%s", dir_name);
            tree_entry.mode = 040000;

            result = build_tree_for_prefix(archive, repo_root, staged, child_prefix, depth + 1, tree_entry.oid, NULL);
            if (result != GIT_RESULT_OK)
                goto cleanup;

            result = tree_list_push(&children, &tree_entry);
            if (result != GIT_RESULT_OK)
                goto cleanup;
        }
    }

    tree_list_sort(&children);

    for (i = 0; i < children.count; i++) {
        const TreeEntry *entry = &children.items[i];
        char mode_text[16];

        if (entry->mode == 040000)
            snprintf(mode_text, sizeof(mode_text), "40000");
        else
            snprintf(mode_text, sizeof(mode_text), "%o", (unsigned int)entry->mode);

        tree_size += strlen(mode_text) + 1 + strlen(entry->name) + 1 + GIT_SHA1_DIGEST_SIZE;
    }

    tree_data = (unsigned char *)malloc((tree_size > 0) ? tree_size : 1);
    if (!tree_data) {
        result = GIT_RESULT_IO_ERROR;
        goto cleanup;
    }

    for (i = 0; i < children.count; i++) {
        const TreeEntry *entry = &children.items[i];
        char mode_text[16];
        size_t mode_len;
        size_t name_len;

        if (entry->mode == 040000)
            snprintf(mode_text, sizeof(mode_text), "40000");
        else
            snprintf(mode_text, sizeof(mode_text), "%o", (unsigned int)entry->mode);

        mode_len = strlen(mode_text);
        name_len = strlen(entry->name);

        memcpy(tree_data + offset, mode_text, mode_len);
        offset += mode_len;

        tree_data[offset++] = ' ';

        memcpy(tree_data + offset, entry->name, name_len);
        offset += name_len;

        tree_data[offset++] = '\0';

        memcpy(tree_data + offset, entry->oid, GIT_SHA1_DIGEST_SIZE);
        offset += GIT_SHA1_DIGEST_SIZE;
    }

    result = write_loose_object(archive, repo_root, "tree", tree_data, tree_size, out_tree_oid, out_tree_oid_hex);

cleanup:
    free(tree_data);
    tree_list_free(&children);
    return result;
}

static GitResult resolve_head_info(FS_Archive archive, const char *repo_root, char *out_ref_path, size_t out_ref_path_len,
    char *out_parent_oid, size_t out_parent_oid_len) {
    char head_path[GIT_PATH_MAX];
    char head_text[256];
    GitResult result;

    if (!repo_root || !out_ref_path || out_ref_path_len == 0 || !out_parent_oid ||
        out_parent_oid_len < (GIT_OID_HEX_LEN + 1))
        return GIT_RESULT_INVALID_ARG;

    out_ref_path[0] = '\0';
    out_parent_oid[0] = '\0';

    if (!join_path(repo_root, ".git/HEAD", head_path, sizeof(head_path)))
        return GIT_RESULT_BUFFER_TOO_SMALL;

    result = read_text_file(archive, head_path, head_text, sizeof(head_text));
    if (result != GIT_RESULT_OK && result != GIT_RESULT_BUFFER_TOO_SMALL)
        return result;

    trim_line_end(head_text);

    if (strncmp(head_text, "ref: ", 5) == 0) {
        const char *ref_path = head_text + 5;
        char refs_root[GIT_PATH_MAX];
        char ref_abs_path[GIT_PATH_MAX];

        while (*ref_path == ' ')
            ref_path++;

        if (*ref_path == '\0')
            return GIT_RESULT_PARSE_ERROR;

        if (snprintf(out_ref_path, out_ref_path_len, "%s", ref_path) <= 0 ||
            strlen(out_ref_path) >= out_ref_path_len) {
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }

        if (!join_path(repo_root, ".git", refs_root, sizeof(refs_root)) ||
            !join_path(refs_root, ref_path, ref_abs_path, sizeof(ref_abs_path))) {
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }

        if (file_exists(archive, ref_abs_path)) {
            char parent_oid[128];
            GitResult parent_result = read_text_file(archive, ref_abs_path, parent_oid, sizeof(parent_oid));
            if (parent_result == GIT_RESULT_OK || parent_result == GIT_RESULT_BUFFER_TOO_SMALL) {
                trim_line_end(parent_oid);
                if (oid_hex_is_valid(parent_oid)) {
                    memcpy(out_parent_oid, parent_oid, GIT_OID_HEX_LEN);
                    out_parent_oid[GIT_OID_HEX_LEN] = '\0';
                }
            }
        }

        return GIT_RESULT_OK;
    }

    if (oid_hex_is_valid(head_text)) {
        snprintf(out_parent_oid, out_parent_oid_len, "%s", head_text);
        return GIT_RESULT_OK;
    }

    return GIT_RESULT_PARSE_ERROR;
}

static GitResult update_head_or_ref(FS_Archive archive, const char *repo_root, const char *ref_path, const char *commit_oid_hex) {
    char content[64];

    if (!repo_root || !commit_oid_hex)
        return GIT_RESULT_INVALID_ARG;

    if (ref_path && ref_path[0] != '\0') {
        char git_root[GIT_PATH_MAX];
        char ref_abs_path[GIT_PATH_MAX];
        char ref_parent[GIT_PATH_MAX];

        if (!join_path(repo_root, ".git", git_root, sizeof(git_root)) ||
            !join_path(git_root, ref_path, ref_abs_path, sizeof(ref_abs_path))) {
            return GIT_RESULT_BUFFER_TOO_SMALL;
        }

        snprintf(ref_parent, sizeof(ref_parent), "%s", ref_abs_path);
        parent_dir(ref_parent);

        if (ensure_directory_recursive(archive, ref_parent) != GIT_RESULT_OK)
            return GIT_RESULT_IO_ERROR;

        snprintf(content, sizeof(content), "%s\n", commit_oid_hex);
        return write_text_file(archive, ref_abs_path, content, true);
    }
    else {
        char head_path[GIT_PATH_MAX];

        if (!join_path(repo_root, ".git/HEAD", head_path, sizeof(head_path)))
            return GIT_RESULT_BUFFER_TOO_SMALL;

        snprintf(content, sizeof(content), "%s\n", commit_oid_hex);
        return write_text_file(archive, head_path, content, true);
    }
}

static GitResult clear_stage_index(FS_Archive archive, const char *repo_root) {
    char index_path[GIT_PATH_MAX];

    if (!repo_root)
        return GIT_RESULT_INVALID_ARG;

    if (!join_path(repo_root, GIT_INDEX_FILE, index_path, sizeof(index_path)))
        return GIT_RESULT_BUFFER_TOO_SMALL;

    return write_file_bytes(archive, index_path, (const unsigned char *)"", 0, true);
}

GitResult git_get_staged_count(const char *start_path, int *out_staged_count) {
    FS_Archive archive;
    char repo_root[GIT_PATH_MAX];
    StageList staged;
    GitResult result;

    if (!out_staged_count)
        return GIT_RESULT_INVALID_ARG;

    *out_staged_count = 0;

    if (!git_find_repository_root(start_path, repo_root, sizeof(repo_root)))
        return GIT_RESULT_NO_REPOSITORY;

    if (!open_sdmc_archive(&archive))
        return GIT_RESULT_IO_ERROR;

    stage_list_init(&staged);
    result = load_stage_index(archive, repo_root, &staged);

    if (result == GIT_RESULT_NO_CHANGES) {
        stage_list_free(&staged);
        close_sdmc_archive(archive);
        return GIT_RESULT_OK;
    }

    if (result != GIT_RESULT_OK) {
        stage_list_free(&staged);
        close_sdmc_archive(archive);
        return result;
    }

    *out_staged_count = (int)staged.count;

    stage_list_free(&staged);
    close_sdmc_archive(archive);
    return GIT_RESULT_OK;
}

GitResult git_add_all(const char *start_path, char *out_message, size_t out_message_len) {
    FS_Archive archive;
    char repo_root[GIT_PATH_MAX];
    StageList staged;
    GitResult result;

    if (!git_find_repository_root(start_path, repo_root, sizeof(repo_root))) {
        set_message(out_message, out_message_len, "No repository found");
        return GIT_RESULT_NO_REPOSITORY;
    }

    if (!open_sdmc_archive(&archive)) {
        set_message(out_message, out_message_len, "Failed to open SD archive");
        return GIT_RESULT_IO_ERROR;
    }

    stage_list_init(&staged);

    result = scan_directory_for_add(archive, repo_root, repo_root, "", 0, &staged);
    if (result != GIT_RESULT_OK) {
        stage_list_free(&staged);
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Failed while scanning files");
        return result;
    }

    stage_list_sort(&staged);
    result = write_stage_index(archive, repo_root, &staged);

    if (result == GIT_RESULT_OK) {
        char message[96];
        snprintf(message, sizeof(message), "Staged %lu file(s)", (unsigned long)staged.count);
        set_message(out_message, out_message_len, message);
    }
    else {
        set_message(out_message, out_message_len, "Failed to write staging index");
    }

    stage_list_free(&staged);
    close_sdmc_archive(archive);
    return result;
}

GitResult git_commit(const char *start_path, const char *message, char *out_commit_oid, size_t out_commit_oid_len,
    char *out_message, size_t out_message_len) {
    FS_Archive archive;
    char repo_root[GIT_PATH_MAX];
    StageList staged;
    GitResult result;
    unsigned char root_tree_oid[GIT_SHA1_DIGEST_SIZE];
    char root_tree_oid_hex[GIT_OID_HEX_LEN + 1];
    char ref_path[GIT_PATH_MAX];
    char parent_oid_hex[GIT_OID_HEX_LEN + 1];
    char author_ident[128];
    char commit_oid_hex[GIT_OID_HEX_LEN + 1];
    unsigned char commit_oid[GIT_SHA1_DIGEST_SIZE];
    char *commit_payload = NULL;
    size_t commit_payload_cap;
    int commit_written;

    if (out_commit_oid && out_commit_oid_len > 0)
        out_commit_oid[0] = '\0';

    if (is_blank_text(message)) {
        set_message(out_message, out_message_len, "Commit message is empty");
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

    stage_list_init(&staged);

    result = load_stage_index(archive, repo_root, &staged);
    if (result == GIT_RESULT_NO_CHANGES) {
        stage_list_free(&staged);
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "No staged changes");
        return GIT_RESULT_NO_CHANGES;
    }

    if (result != GIT_RESULT_OK) {
        stage_list_free(&staged);
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot parse staging index");
        return result;
    }

    result = build_tree_for_prefix(archive, repo_root, &staged, "", 0, root_tree_oid, root_tree_oid_hex);
    if (result != GIT_RESULT_OK) {
        stage_list_free(&staged);
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot build tree object");
        return result;
    }

    result = resolve_head_info(archive, repo_root, ref_path, sizeof(ref_path), parent_oid_hex, sizeof(parent_oid_hex));
    if (result != GIT_RESULT_OK) {
        stage_list_free(&staged);
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot resolve HEAD reference");
        return result;
    }

    snprintf(author_ident, sizeof(author_ident), "3DS CodEdit <codedit@3ds.local> %ld +0000", (long)time(NULL));

    commit_payload_cap = strlen(message) + 512;
    commit_payload = (char *)malloc(commit_payload_cap);
    if (!commit_payload) {
        stage_list_free(&staged);
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Out of memory while creating commit");
        return GIT_RESULT_IO_ERROR;
    }

    if (parent_oid_hex[0] != '\0') {
        commit_written = snprintf(commit_payload, commit_payload_cap,
            "tree %s\n"
            "parent %s\n"
            "author %s\n"
            "committer %s\n"
            "\n"
            "%s\n",
            root_tree_oid_hex,
            parent_oid_hex,
            author_ident,
            author_ident,
            message);
    }
    else {
        commit_written = snprintf(commit_payload, commit_payload_cap,
            "tree %s\n"
            "author %s\n"
            "committer %s\n"
            "\n"
            "%s\n",
            root_tree_oid_hex,
            author_ident,
            author_ident,
            message);
    }

    if (commit_written <= 0 || (size_t)commit_written >= commit_payload_cap) {
        free(commit_payload);
        stage_list_free(&staged);
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Commit payload too large");
        return GIT_RESULT_BUFFER_TOO_SMALL;
    }

    result = write_loose_object(archive, repo_root, "commit", (const unsigned char *)commit_payload,
        (size_t)commit_written, commit_oid, commit_oid_hex);
    free(commit_payload);

    if (result != GIT_RESULT_OK) {
        stage_list_free(&staged);
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot write commit object");
        return result;
    }

    result = update_head_or_ref(archive, repo_root, ref_path, commit_oid_hex);
    if (result != GIT_RESULT_OK) {
        stage_list_free(&staged);
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Cannot update branch reference");
        return result;
    }

    result = clear_stage_index(archive, repo_root);
    if (result != GIT_RESULT_OK) {
        stage_list_free(&staged);
        close_sdmc_archive(archive);
        set_message(out_message, out_message_len, "Commit created, but failed to clear staging area");
        return result;
    }

    if (out_commit_oid && out_commit_oid_len > 0)
        snprintf(out_commit_oid, out_commit_oid_len, "%s", commit_oid_hex);

    set_message(out_message, out_message_len, "Commit created");

    stage_list_free(&staged);
    close_sdmc_archive(archive);
    return GIT_RESULT_OK;
}
