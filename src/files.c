#include "coarena.h"

/* Safe storage of generated files and attachment downloads. */
int write_all_fd(int fd, const unsigned char *data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = write(fd, data + offset, len - offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (n == 0) return 0;
        offset += (size_t)n;
    }
    return 1;
}

void safe_filename(const char *suggested, char *out, size_t out_size)
{
    const char *base = suggested ? suggested : "";
    const char *slash1 = strrchr(base, '/');
    const char *slash2 = strrchr(base, '\\');
    size_t i, j = 0;
    if (slash1 && (!slash2 || slash1 > slash2)) base = slash1 + 1;
    else if (slash2) base = slash2 + 1;
    if (!*base || !strcmp(base, ".") || !strcmp(base, "..")) base = "returned_file.bin";
    for (i = 0; base[i] && j + 1 < out_size && j < 120; ++i) {
        unsigned char c = (unsigned char)base[i];
        if (isalnum(c) || c == '.' || c == '_' || c == '-') out[j++] = (char)c;
        else if (c == ' ' || c == '\t') out[j++] = '_';
    }
    out[j] = '\0';
    while (j && out[j - 1] == '.') out[--j] = '\0';
    if (!j || out[0] == '.') snprintf(out, out_size, "returned_file.bin");
}

int write_unique_file(const char *directory, const char *suggested,
                             const unsigned char *data, size_t len,
                             char *saved_name, size_t saved_name_size)
{
    char stamp[32], clean[128], path[PATH_MAX];
    time_t now = time(NULL);
    int fd = -1;
    unsigned int suffix;

    if (!format_timestamp(now, stamp, sizeof(stamp))) strcpy(stamp, "file");
    safe_filename(suggested, clean, sizeof(clean));

    for (suffix = 0; suffix < 10000; ++suffix) {
        int n;
        if (suffix == 0)
            n = snprintf(saved_name, saved_name_size, "%s_%s", stamp, clean);
        else
            n = snprintf(saved_name, saved_name_size, "%s_%u_%s", stamp, suffix, clean);
        if (n < 0 || (size_t)n >= saved_name_size) return 0;
        if (snprintf(path, sizeof(path), "%s/%s", directory, saved_name) >= (int)sizeof(path))
            return 0;
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) break;
        if (errno != EEXIST) {
            fprintf(stderr, "Cannot create returned file '%s': %s\n", path, strerror(errno));
            return 0;
        }
    }
    if (fd < 0) {
        fprintf(stderr, "Could not generate a unique filename in %s.\n", directory);
        return 0;
    }
    if (!write_all_fd(fd, data, len) || fsync(fd) != 0) {
        int saved_errno = errno;
        close(fd);
        unlink(path);
        errno = saved_errno;
        fprintf(stderr, "Could not write returned file '%s': %s\n", path, strerror(errno));
        return 0;
    }
    if (close(fd) != 0) {
        unlink(path);
        fprintf(stderr, "Could not close returned file '%s': %s\n", path, strerror(errno));
        return 0;
    }
    return 1;
}

int base64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int base64_decode(const char *src, struct buffer *out)
{
    unsigned int accumulator = 0;
    int bits = 0;
    size_t i;
    for (i = 0; src[i]; ++i) {
        int value;
        unsigned char c = (unsigned char)src[i];
        if (c == '=') break;
        if (isspace(c)) continue;
        value = base64_value(c);
        if (value < 0) return 0;
        accumulator = (accumulator << 6) | (unsigned int)value;
        bits += 6;
        if (bits >= 8) {
            unsigned char byte;
            bits -= 8;
            byte = (unsigned char)((accumulator >> bits) & 0xffU);
            if (out->len >= MAX_RETURNED_FILE_SIZE || !buffer_append(out, &byte, 1)) return 0;
        }
    }
    return 1;
}

size_t bounded_download_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct download_context *ctx = userdata;
    size_t total = size * nmemb;
    if (total > MAX_RETURNED_FILE_SIZE - ctx->bytes.len) {
        ctx->too_large = 1;
        return 0;
    }
    return buffer_append(&ctx->bytes, ptr, total) ? total : 0;
}

int download_https_file(const char *url, struct buffer *bytes)
{
    CURL *curl;
    CURLcode result;
    long http_code = 0;
    struct download_context ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (!url || strncmp(url, "https://", 8) != 0) {
        fprintf(stderr, "Returned file URL was not HTTPS; refusing to download it.\n");
        return 0;
    }
    curl = curl_easy_init();
    if (!curl) return 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bounded_download_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "coarena-c-client/1.1");
    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK || http_code < 200 || http_code >= 300 || ctx.too_large) {
        if (ctx.too_large)
            fprintf(stderr, "Returned file exceeded the %u MiB safety limit.\n",
                    (unsigned int)(MAX_RETURNED_FILE_SIZE / (1024U * 1024U)));
        else
            fprintf(stderr, "Could not download returned file (HTTP %ld): %s\n",
                    http_code, result == CURLE_OK ? "HTTP error" : curl_easy_strerror(result));
        buffer_free(&ctx.bytes);
        return 0;
    }
    *bytes = ctx.bytes;
    return 1;
}

int save_json_file_object(json_object *file_obj, const char *directory)
{
    static const char *const name_keys[] = {"filename", "file_name", "fileName", "name", "path"};
    static const char *const content_keys[] = {"content", "text", "body", "data"};
    static const char *const b64_keys[] = {"base64", "data_base64", "content_base64", "file_data", "fileData"};
    static const char *const url_keys[] = {"url", "download_url", "downloadUrl", "file_url", "href"};
    const char *name, *content, *encoded, *url, *encoding;
    json_object *payload = file_obj, *nested = NULL, *payload_media_type = NULL;
    int payload_has_media_type = 0;
    struct buffer bytes = {0};
    char saved_name[256];
    int ok = 0;

    if (!file_obj || json_object_get_type(file_obj) != json_type_object) return 0;
    if ((!json_object_object_get_ex(file_obj, "content", &nested) || !nested) &&
        (!json_object_object_get_ex(file_obj, "data", &nested) || !nested)) {
        if (json_object_object_get_ex(file_obj, "source", &nested) && nested &&
            json_object_get_type(nested) == json_type_object) payload = nested;
        else if (json_object_object_get_ex(file_obj, "image_url", &nested) && nested &&
                 json_object_get_type(nested) == json_type_object) payload = nested;
        else if (json_object_object_get_ex(file_obj, "file", &nested) && nested &&
                 json_object_get_type(nested) == json_type_object) payload = nested;
    }
    if (payload != file_obj)
        payload_has_media_type = json_object_object_get_ex(payload, "media_type", &payload_media_type);
    name = json_string_for_keys(file_obj, name_keys, sizeof(name_keys) / sizeof(name_keys[0]));
    if (!name && payload != file_obj)
        name = json_string_for_keys(payload, name_keys, sizeof(name_keys) / sizeof(name_keys[0]));
    content = json_string_for_keys(payload, content_keys, sizeof(content_keys) / sizeof(content_keys[0]));
    encoded = json_string_for_keys(payload, b64_keys, sizeof(b64_keys) / sizeof(b64_keys[0]));
    url = json_string_for_keys(payload, url_keys, sizeof(url_keys) / sizeof(url_keys[0]));
    {
        static const char *const encoding_keys[] = {"encoding", "content_encoding", "data_encoding"};
        encoding = json_string_for_keys(payload, encoding_keys,
                                        sizeof(encoding_keys) / sizeof(encoding_keys[0]));
    }

    if (encoded) {
        if (!base64_decode(encoded, &bytes)) goto done;
    } else if (content) {
        if (!strncmp(content, "data:", 5)) {
            const char *comma = strchr(content, ',');
            if (!comma || !strstr(content, ";base64")) goto done;
            if (!base64_decode(comma + 1, &bytes)) goto done;
        } else if ((encoding && !strcasecmp(encoding, "base64")) ||
                   (payload_has_media_type && payload_media_type &&
                    json_object_get_type(payload_media_type) == json_type_string)) {
            if (!base64_decode(content, &bytes)) goto done;
        } else {
            size_t len = strlen(content);
            if (len > MAX_RETURNED_FILE_SIZE || !buffer_append(&bytes, content, len)) goto done;
        }
    } else if (url) {
        if (!download_https_file(url, &bytes)) goto done;
    } else {
        /* Some provider payloads use {name, data, encoding}. */
        json_object *data_obj = NULL;
        if (!json_object_object_get_ex(file_obj, "data", &data_obj) ||
            !data_obj || json_object_get_type(data_obj) != json_type_string) return 0;
        content = json_object_get_string(data_obj);
        if (encoding && !strcasecmp(encoding, "base64")) {
            if (!base64_decode(content, &bytes)) goto done;
        } else {
            size_t len = strlen(content);
            if (len > MAX_RETURNED_FILE_SIZE || !buffer_append(&bytes, content, len)) goto done;
        }
    }

    if (!name) {
        json_object *type_obj = NULL, *mime_obj = NULL;
        if (json_object_object_get_ex(file_obj, "type", &type_obj) &&
            json_object_get_type(type_obj) == json_type_string &&
            !strcmp(json_object_get_string(type_obj), "image")) {
            const char *mime = NULL;
            if (payload != file_obj && json_object_object_get_ex(payload, "media_type", &mime_obj) &&
                json_object_get_type(mime_obj) == json_type_string)
                mime = json_object_get_string(mime_obj);
            name = mime && strstr(mime, "jpeg") ? "returned_image.jpg" :
                   mime && strstr(mime, "webp") ? "returned_image.webp" :
                   mime && strstr(mime, "gif") ? "returned_image.gif" : "returned_image.png";
        }
    }
    if (write_unique_file(directory, name ? name : "returned_file.bin",
                          (const unsigned char *)(bytes.data ? bytes.data : ""),
                          bytes.len, saved_name, sizeof(saved_name))) {
        if (isatty(STDOUT_FILENO))
            printf("\n\033[32m[Saved returned file] %s/\033[1m%s\033[0m\n",
                   "CoarenaFiles", saved_name);
        else
            printf("\n[Saved returned file] %s/%s\n", "CoarenaFiles", saved_name);
        ok = 1;
    }
done:
    buffer_free(&bytes);
    return ok;
}

int save_structured_returned_files(json_object *files, const char *directory)
{
    size_t i;
    int saved = 0;
    if (!files || json_object_get_type(files) != json_type_array) return 0;
    for (i = 0; i < json_object_array_length(files); ++i) {
        if (save_json_file_object(json_object_array_get_idx(files, i), directory)) ++saved;
    }
    return saved;
}

int line_is_file_fence(const char *line, size_t len, char *name, size_t name_size)
{
    size_t start = 0, end = len, marker_len;
    const char *marker;
    while (start < end && (line[start] == ' ' || line[start] == '\t')) ++start;
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r')) --end;
    if (end - start < 7 || memcmp(line + start, "```", 3) != 0) return 0;
    marker = line + start + 3;
    marker_len = end - start - 3;
    if (marker_len > 5 && !strncasecmp(marker, "file:", 5)) {
        marker += 5;
        marker_len -= 5;
    } else if (marker_len > 5 && !strncasecmp(marker, "file ", 5)) {
        marker += 5;
        marker_len -= 5;
    } else return 0;
    while (marker_len && (*marker == ' ' || *marker == '\t')) {
        ++marker;
        --marker_len;
    }
    if (!marker_len || marker_len >= name_size) return 0;
    memcpy(name, marker, marker_len);
    name[marker_len] = '\0';
    return 1;
}

int line_is_closing_fence(const char *line, size_t len)
{
    size_t start = 0, end = len;
    while (start < end && (line[start] == ' ' || line[start] == '\t')) ++start;
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r')) --end;
    return end - start >= 3 && memcmp(line + start, "```", 3) == 0;
}

char *save_markdown_file_blocks(const char *answer, const char *directory)
{
    struct buffer display = {0};
    size_t pos = 0, total = strlen(answer);
    while (pos < total) {
        size_t line_start = pos, line_end;
        char filename[256];
        while (pos < total && answer[pos] != '\n') ++pos;
        line_end = pos;
        if (line_is_file_fence(answer + line_start, line_end - line_start,
                               filename, sizeof(filename))) {
            size_t content_start = pos < total ? pos + 1 : pos;
            size_t scan = content_start, close_start = total, close_end = total;
            while (scan < total) {
                size_t candidate_start = scan, candidate_end;
                while (scan < total && answer[scan] != '\n') ++scan;
                candidate_end = scan;
                if (line_is_closing_fence(answer + candidate_start,
                                          candidate_end - candidate_start)) {
                    close_start = candidate_start;
                    close_end = scan < total ? scan + 1 : scan;
                    break;
                }
                if (scan < total) ++scan;
            }
            if (close_start < total) {
                char saved[256];
                if (close_start - content_start > MAX_RETURNED_FILE_SIZE) {
                    fprintf(stderr, "Model-generated file '%s' exceeded the %u MiB safety limit; leaving it in the response.\n",
                            filename, (unsigned int)(MAX_RETURNED_FILE_SIZE / (1024U * 1024U)));
                    buffer_append(&display, answer + line_start, close_end - line_start);
                } else if (write_unique_file(directory, filename,
                        (const unsigned char *)(answer + content_start),
                        close_start - content_start, saved, sizeof(saved))) {
                    char notice[512];
                    int n = snprintf(notice, sizeof(notice),
                                     "**File saved:** `CoarenaFiles/%s`\n", saved);
                    if (n > 0) buffer_append(&display, notice, (size_t)n);
                } else {
                    buffer_append(&display, answer + line_start, close_end - line_start);
                }
                pos = close_end;
                continue;
            }
        }
        buffer_append(&display, answer + line_start, line_end - line_start);
        if (pos < total) {
            buffer_append(&display, "\n", 1);
            ++pos;
        }
    }
    if (!display.data) return strdup("");
    return display.data;
}

void print_formatted_answer(const char *answer, int color)
{
    char *display = save_markdown_file_blocks(answer, files_dir);
    if (!display) display = strdup(answer);
    fputs("\n", stdout);
    render_markdown(display ? display : answer, color);
    if (display) free(display);
    fflush(stdout);
}

