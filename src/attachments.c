#include "coarena.h"

/* User message construction and local attachment encoding. */
const char *image_mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    char ext[16];
    size_t i, n;

    if (!dot) {
        return NULL;
    }
    ++dot;
    n = strlen(dot);
    if (n == 0 || n >= sizeof(ext)) {
        return NULL;
    }
    for (i = 0; i < n; ++i) {
        ext[i] = (char)tolower((unsigned char)dot[i]);
    }
    ext[n] = '\0';

    if (!strcmp(ext, "jpg") || !strcmp(ext, "jpeg")) return "image/jpeg";
    if (!strcmp(ext, "png")) return "image/png";
    if (!strcmp(ext, "gif")) return "image/gif";
    if (!strcmp(ext, "webp")) return "image/webp";
    return NULL;
}

char *base64_encode(const unsigned char *src, size_t len)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    size_t i = 0, j = 0;

    if (!out) return NULL;

    while (i + 3 <= len) {
        unsigned int v = ((unsigned int)src[i] << 16) |
                         ((unsigned int)src[i + 1] << 8) |
                         (unsigned int)src[i + 2];
        out[j++] = table[(v >> 18) & 63];
        out[j++] = table[(v >> 12) & 63];
        out[j++] = table[(v >> 6) & 63];
        out[j++] = table[v & 63];
        i += 3;
    }

    if (i < len) {
        unsigned int v = (unsigned int)src[i] << 16;
        out[j++] = table[(v >> 18) & 63];
        if (i + 1 < len) {
            v |= (unsigned int)src[i + 1] << 8;
            out[j++] = table[(v >> 12) & 63];
            out[j++] = table[(v >> 6) & 63];
            out[j++] = '=';
        } else {
            out[j++] = table[(v >> 12) & 63];
            out[j++] = '=';
            out[j++] = '=';
        }
    }
    out[j] = '\0';
    return out;
}

int parse_attachment_command(const char *line, const char *command,
                                    char **path_out, char **prompt_out)
{
    const char *p = line + strlen(command);
    const char *path_start;
    const char *path_end;
    const char *separator;
    size_t path_len;
    char *path;
    char *prompt;

    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p == '"') {
        path_start = ++p;
        while (*p && *p != '"') ++p;
        if (*p != '"') {
            fprintf(stderr, "Close the quoted attachment path with a double quote.\n");
            return -1;
        }
        path_end = p;
        ++p;
        while (*p && isspace((unsigned char)*p)) ++p;
        if (*p != '|') {
            fprintf(stderr, "Attachment syntax: %s \"path\" | your prompt\n", command);
            return -1;
        }
        separator = p;
    } else {
        path_start = p;
        separator = strchr(p, '|');
        if (!separator) {
            fprintf(stderr, "Attachment syntax: %s path | your prompt\n", command);
            return -1;
        }
        path_end = separator;
        while (path_end > path_start && isspace((unsigned char)path_end[-1])) {
            --path_end;
        }
    }

    path_len = (size_t)(path_end - path_start);
    if (path_len == 0) {
        fprintf(stderr, "Attachment path is empty.\n");
        return -1;
    }

    path = malloc(path_len + 1);
    if (!path) return -1;
    memcpy(path, path_start, path_len);
    path[path_len] = '\0';

    prompt = strdup(separator + 1);
    if (!prompt) {
        free(path);
        return -1;
    }
    {
        char *clean = trim_whitespace(prompt);
        if (clean != prompt) memmove(prompt, clean, strlen(clean) + 1);
    }
    if (!prompt[0]) {
        free(prompt);
        prompt = strdup("Please inspect the attached file and explain what you find.");
        if (!prompt) {
            free(path);
            return -1;
        }
    }

    *path_out = path;
    *prompt_out = prompt;
    return 1;
}

json_object *make_user_message(const char *line)
{
    json_object *message = NULL;
    json_object *content = NULL;
    char *path = NULL;
    char *prompt = NULL;
    int parsed;

    if (!strncmp(line, "/image", 6) &&
        (line[6] == '\0' || isspace((unsigned char)line[6]))) {
        struct buffer file = {0};
        char *encoded = NULL;
        const char *mime;
        struct buffer uri = {0};
        json_object *text_part, *image_part, *image_url;

        parsed = parse_attachment_command(line, "/image", &path, &prompt);
        if (parsed <= 0) return NULL;
        mime = image_mime_type(path);
        if (!mime) {
            fprintf(stderr, "Supported image extensions: .jpg, .jpeg, .png, .gif, .webp\n");
            goto image_fail;
        }
        if (!read_file(path, MAX_IMAGE_SIZE, &file)) goto image_fail;
        if (file.len == 0) {
            fprintf(stderr, "The image file is empty.\n");
            goto image_fail;
        }
        encoded = base64_encode((const unsigned char *)file.data, file.len);
        if (!encoded) {
            fprintf(stderr, "Could not encode image (out of memory).\n");
            goto image_fail;
        }
        if (!buffer_append(&uri, "data:", 5) ||
            !buffer_append(&uri, mime, strlen(mime)) ||
            !buffer_append(&uri, ";base64,", 8) ||
            !buffer_append(&uri, encoded, strlen(encoded))) {
            fprintf(stderr, "Out of memory while preparing image.\n");
            goto image_fail;
        }

        content = json_object_new_array();
        text_part = json_object_new_object();
        image_part = json_object_new_object();
        image_url = json_object_new_object();
        if (!content || !text_part || !image_part || !image_url) {
            if (content) json_object_put(content);
            if (text_part) json_object_put(text_part);
            if (image_part) json_object_put(image_part);
            if (image_url) json_object_put(image_url);
            content = NULL;
            goto image_fail;
        }

        json_object_object_add(text_part, "type", json_object_new_string("text"));
        json_object_object_add(text_part, "text", json_object_new_string(prompt));
        json_object_object_add(image_url, "url", json_object_new_string(uri.data));
        json_object_object_add(image_url, "detail", json_object_new_string("auto"));
        json_object_object_add(image_part, "type", json_object_new_string("image_url"));
        json_object_object_add(image_part, "image_url", image_url);
        json_object_array_add(content, text_part);
        json_object_array_add(content, image_part);

        message = json_object_new_object();
        if (!message) {
            json_object_put(content);
            content = NULL;
            goto image_fail;
        }
        json_object_object_add(message, "role", json_object_new_string("user"));
        json_object_object_add(message, "content", content);

        free(encoded);
        buffer_free(&uri);
        buffer_free(&file);
        free(path);
        free(prompt);
        return message;

image_fail:
        free(encoded);
        buffer_free(&uri);
        buffer_free(&file);
        free(path);
        free(prompt);
        return NULL;
    }

    if (!strncmp(line, "/file", 5) &&
        (line[5] == '\0' || isspace((unsigned char)line[5]))) {
        struct buffer file = {0};
        struct buffer combined = {0};
        parsed = parse_attachment_command(line, "/file", &path, &prompt);
        if (parsed <= 0) return NULL;
        if (!read_file(path, MAX_TEXT_SIZE, &file)) goto file_fail;
        if (file.len && memchr(file.data, '\0', file.len)) {
            fprintf(stderr, "The /file command accepts text files, not binary files.\n");
            goto file_fail;
        }
        if (!buffer_append(&combined, prompt, strlen(prompt)) ||
            !buffer_append(&combined, "\n\n--- BEGIN ATTACHED FILE: ", 27) ||
            !buffer_append(&combined, path, strlen(path)) ||
            !buffer_append(&combined, " ---\n", 5) ||
            !buffer_append(&combined, file.data ? file.data : "", file.len) ||
            !buffer_append(&combined, "\n--- END ATTACHED FILE ---", 26)) {
            fprintf(stderr, "Out of memory while preparing text attachment.\n");
            goto file_fail;
        }

        message = json_object_new_object();
        if (!message) goto file_fail;
        json_object_object_add(message, "role", json_object_new_string("user"));
        json_object_object_add(message, "content",
                               json_object_new_string_len(combined.data,
                                                          (int)combined.len));
        buffer_free(&combined);
        buffer_free(&file);
        free(path);
        free(prompt);
        return message;

file_fail:
        buffer_free(&combined);
        buffer_free(&file);
        free(path);
        free(prompt);
        return NULL;
    }

    message = json_object_new_object();
    if (!message) return NULL;
    json_object_object_add(message, "role", json_object_new_string("user"));
    json_object_object_add(message, "content", json_object_new_string(line));
    return message;
}

