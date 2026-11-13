#include "coarena.h"

/* Coarena HTTP API, streaming, model catalogue, and key routing. */
void append_and_print(struct stream_context *ctx, const char *text, size_t len)
{
    if (!len) return;
    if (!buffer_append(&ctx->answer, text, len)) {
        fprintf(stderr, "\nOut of memory while receiving the answer.\n");
        ctx->api_error = 1;
    }
}

void handle_delta_content(struct stream_context *ctx, json_object *content)
{
    enum json_type type;

    if (!content) return;
    type = json_object_get_type(content);
    if (type == json_type_string) {
        const char *s = json_object_get_string(content);
        append_and_print(ctx, s, strlen(s));
    } else if (type == json_type_array) {
        size_t i, n = json_object_array_length(content);
        for (i = 0; i < n; ++i) {
            json_object *part = json_object_array_get_idx(content, i);
            json_object *text_obj = NULL;
            if (json_object_object_get_ex(part, "text", &text_obj) &&
                json_object_get_type(text_obj) == json_type_string) {
                const char *s = json_object_get_string(text_obj);
                append_and_print(ctx, s, strlen(s));
            }
        }
    }
}

void collect_file_fields(json_object *source, json_object *files)
{
    static const char *const names[] = {
        "files", "attachments", "artifacts", "file", "attachment", "artifact"
    };
    size_t i;
    if (!source || !files || json_object_get_type(source) != json_type_object) return;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        json_object *value = NULL;
        if (!json_object_object_get_ex(source, names[i], &value) || !value) continue;
        if (json_object_get_type(value) == json_type_array) {
            size_t j;
            for (j = 0; j < json_object_array_length(value); ++j) {
                json_object *item = json_object_array_get_idx(value, j);
                if (item) json_object_array_add(files, json_object_get(item));
            }
        } else if (json_object_get_type(value) == json_type_object) {
            json_object_array_add(files, json_object_get(value));
        }
    }
}

void collect_typed_file_parts(json_object *content, json_object *files)
{
    size_t i;
    if (!content || !files) return;
    if (json_object_get_type(content) == json_type_array) {
        for (i = 0; i < json_object_array_length(content); ++i)
            collect_typed_file_parts(json_object_array_get_idx(content, i), files);
        return;
    }
    if (json_object_get_type(content) == json_type_object) {
        json_object *type_obj = NULL;
        if (json_object_object_get_ex(content, "type", &type_obj) &&
            json_object_get_type(type_obj) == json_type_string) {
            const char *type = json_object_get_string(type_obj);
            if (!strcmp(type, "file") || !strcmp(type, "image") ||
                !strcmp(type, "document") || !strcmp(type, "artifact") ||
                !strcmp(type, "file_attachment") || !strcmp(type, "file_reference") ||
                !strcmp(type, "image_url")) {
                json_object_array_add(files, json_object_get(content));
                return;
            }
        }
    }
}

void process_sse_json(struct stream_context *ctx, const char *json_text)
{
    json_object *root;
    json_object *error_obj = NULL;
    json_object *choices = NULL;

    root = json_tokener_parse(json_text);
    if (!root) return;

    collect_file_fields(root, ctx->returned_files);

    if (json_object_object_get_ex(root, "error", &error_obj)) {
        json_object *message = NULL;
        const char *s = "Coarena returned an API error.";
        if (json_object_get_type(error_obj) == json_type_object &&
            json_object_object_get_ex(error_obj, "message", &message) &&
            json_object_get_type(message) == json_type_string) {
            s = json_object_get_string(message);
        }
        fprintf(stderr, "\nCoarena API error: %s\n", s);
        ctx->api_error = 1;
        json_object_put(root);
        return;
    }

    {
        json_object *model = NULL;
        if (json_object_object_get_ex(root, "model", &model) &&
            json_object_get_type(model) == json_type_string) {
            snprintf(ctx->actual_model, sizeof(ctx->actual_model), "%s",
                     json_object_get_string(model));
        }
    }

    if (json_object_object_get_ex(root, "choices", &choices) &&
        json_object_get_type(choices) == json_type_array &&
        json_object_array_length(choices) > 0) {
        json_object *choice = json_object_array_get_idx(choices, 0);
        json_object *delta = NULL;
        json_object *content = NULL;
        if (json_object_object_get_ex(choice, "delta", &delta) &&
            json_object_get_type(delta) == json_type_object) {
            collect_file_fields(delta, ctx->returned_files);
            if (json_object_object_get_ex(delta, "content", &content)) {
                collect_typed_file_parts(content, ctx->returned_files);
                handle_delta_content(ctx, content);
            }
        }
        collect_file_fields(choice, ctx->returned_files);
    }

    json_object_put(root);
}

void process_sse_line(struct stream_context *ctx, const char *line, size_t len)
{
    size_t start = 0;

    while (start < len && (line[start] == ' ' || line[start] == '\t')) ++start;
    if (len - start >= 5 && !memcmp(line + start, "data:", 5)) {
        const char *payload = line + start + 5;
        size_t payload_len = len - start - 5;
        while (payload_len && (*payload == ' ' || *payload == '\t')) {
            ++payload;
            --payload_len;
        }
        if (payload_len == 6 && !memcmp(payload, "[DONE]", 6)) return;
        if (payload_len) {
            char *copy = malloc(payload_len + 1);
            if (!copy) {
                ctx->api_error = 1;
                return;
            }
            memcpy(copy, payload, payload_len);
            copy[payload_len] = '\0';
            process_sse_json(ctx, copy);
            free(copy);
        }
    } else if (len) {
        if (!buffer_append(&ctx->raw_body, line, len) ||
            !buffer_append(&ctx->raw_body, "\n", 1)) {
            ctx->api_error = 1;
        }
    }
}

size_t stream_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct stream_context *ctx = userdata;
    size_t total = size * nmemb;
    size_t i, start = 0;

    if (!buffer_append(&ctx->pending, ptr, total)) return 0;

    for (i = 0; i < ctx->pending.len; ++i) {
        if (ctx->pending.data[i] == '\n') {
            size_t line_len = i - start;
            if (line_len && ctx->pending.data[start + line_len - 1] == '\r') {
                --line_len;
            }
            process_sse_line(ctx, ctx->pending.data + start, line_len);
            start = i + 1;
        }
    }

    if (start) {
        size_t remaining = ctx->pending.len - start;
        memmove(ctx->pending.data, ctx->pending.data + start, remaining);
        ctx->pending.len = remaining;
        ctx->pending.data[remaining] = '\0';
    }
    return total;
}

void finish_pending_stream(struct stream_context *ctx)
{
    if (ctx->pending.len) {
        size_t len = ctx->pending.len;
        if (len && ctx->pending.data[len - 1] == '\r') --len;
        process_sse_line(ctx, ctx->pending.data, len);
        buffer_clear(&ctx->pending);
    }
}

size_t header_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct stream_context *ctx = userdata;
    size_t total = size * nmemb;
    char *colon;
    char key[128];
    char *value;
    size_t key_len;

    colon = memchr(ptr, ':', total);
    if (!colon) return total;
    key_len = (size_t)(colon - ptr);
    if (key_len == 0 || key_len >= sizeof(key)) return total;
    memcpy(key, ptr, key_len);
    key[key_len] = '\0';
    for (size_t i = 0; key[i]; ++i) key[i] = (char)tolower((unsigned char)key[i]);

    value = colon + 1;
    while ((size_t)(ptr + total - value) > 0 && (*value == ' ' || *value == '\t')) ++value;
    {
        size_t value_len = (size_t)(ptr + total - value);
        while (value_len && (value[value_len - 1] == '\r' || value[value_len - 1] == '\n')) --value_len;
        if (!strcmp(key, "x-coarena-substituted-from")) {
            snprintf(ctx->substituted_from, sizeof(ctx->substituted_from), "%.*s", (int)value_len, value);
        } else if (!strcmp(key, "x-coarena-substituted-to")) {
            snprintf(ctx->substituted_to, sizeof(ctx->substituted_to), "%.*s", (int)value_len, value);
        } else if (!strcmp(key, "x-coarena-substitution-reason")) {
            snprintf(ctx->substitution_reason, sizeof(ctx->substitution_reason), "%.*s", (int)value_len, value);
        }
    }
    return total;
}

struct curl_slist *make_headers(const char *key)
{
    struct curl_slist *headers = NULL;
    char *authorization = malloc(strlen(key) + 32);
    if (!authorization) return NULL;
    sprintf(authorization, "Authorization: Bearer %s", key);
    headers = curl_slist_append(headers, authorization);
    free(authorization);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream, application/json");
    return headers;
}

void print_http_error(long code, const char *body)
{
    json_object *root = NULL;
    json_object *err = NULL;
    json_object *msg = NULL;

    fprintf(stderr, "\nHTTP error: %ld\n", code);
    if (body && *body) {
        root = json_tokener_parse(body);
        if (root && json_object_object_get_ex(root, "error", &err)) {
            if (json_object_get_type(err) == json_type_object &&
                json_object_object_get_ex(err, "message", &msg) &&
                json_object_get_type(msg) == json_type_string) {
                fprintf(stderr, "%s\n", json_object_get_string(msg));
            } else {
                fprintf(stderr, "%s\n",
                        json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY));
            }
        } else {
            fprintf(stderr, "%s\n", body);
        }
    }
    if (root) json_object_put(root);
}

const char *json_string_for_keys(json_object *obj, const char *const *keys, size_t count)
{
    size_t i;
    json_object *value = NULL;
    for (i = 0; i < count; ++i) {
        if (json_object_object_get_ex(obj, keys[i], &value) && value &&
            json_object_get_type(value) == json_type_string) {
            return json_object_get_string(value);
        }
    }
    return NULL;
}

int call_model(const char *key, json_object *messages, const char *model_id)
{
    CURL *curl = NULL;
    CURLcode result;
    struct curl_slist *headers = NULL;
    struct stream_context ctx;
    json_object *request = NULL;
    const char *body;
    long http_code = 0;
    int success = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.stdout_is_tty = isatty(STDOUT_FILENO);
    ctx.returned_files = json_object_new_array();
    if (!ctx.returned_files) goto cleanup;

    request = json_object_new_object();
    if (!request) goto cleanup;
    json_object_object_add(request, "model", json_object_new_string(model_id));
    json_object_object_add(request, "messages", json_object_get(messages));
    json_object_object_add(request, "stream", json_object_new_boolean(1));

    body = json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN);
    if (!body) goto cleanup;

    curl = curl_easy_init();
    if (!curl) goto cleanup;
    headers = make_headers(key);
    if (!headers) goto cleanup;

    curl_easy_setopt(curl, CURLOPT_URL, API_BASE "/chat/completions");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "coarena-c-client/1.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    fprintf(stdout, "\n[Connecting to Coarena / %s ...]\n", model_id);
    fflush(stdout);
    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    finish_pending_stream(&ctx);

    if (result != CURLE_OK) {
        fprintf(stderr, "\nNetwork error: %s\n", curl_easy_strerror(result));
        goto cleanup;
    }

    if (http_code < 200 || http_code >= 300) {
        print_http_error(http_code, ctx.raw_body.data);
        goto cleanup;
    }

    /* Fallback for servers that return one ordinary JSON completion instead of SSE. */
    if (ctx.answer.len == 0 && ctx.raw_body.len) {
        json_object *root = json_tokener_parse(ctx.raw_body.data);
        json_object *choices = NULL, *choice = NULL, *message = NULL, *content = NULL;
        if (root) collect_file_fields(root, ctx.returned_files);
        if (root && json_object_object_get_ex(root, "choices", &choices) &&
            json_object_get_type(choices) == json_type_array &&
            json_object_array_length(choices) > 0) {
            choice = json_object_array_get_idx(choices, 0);
            collect_file_fields(choice, ctx.returned_files);
            if (json_object_object_get_ex(choice, "message", &message)) {
                collect_file_fields(message, ctx.returned_files);
                if (json_object_object_get_ex(message, "content", &content)) {
                    collect_typed_file_parts(content, ctx.returned_files);
                    handle_delta_content(&ctx, content);
                }
            }
        }
        if (root) json_object_put(root);
    }

    if (ctx.api_error) goto cleanup;
    {
        int saved_files = save_structured_returned_files(ctx.returned_files, files_dir);
        if (ctx.answer.len == 0 && saved_files > 0) {
            const char *notice = "The model returned file attachment(s); see CoarenaFiles/.";
            buffer_append(&ctx.answer, notice, strlen(notice));
        }
    }
    if (ctx.answer.len == 0) {
        fprintf(stderr, "The API returned no text content or recognized file attachment.\n");
        goto cleanup;
    }

    if (ctx.actual_model[0]) {
        fprintf(stdout, "[Model reported by API: %s]\n", ctx.actual_model);
    }
    if (ctx.substituted_to[0]) {
        fprintf(stdout, "[Coarena routing: requested %s; answered with %s (%s)]\n",
                ctx.substituted_from[0] ? ctx.substituted_from : model_id,
                ctx.substituted_to,
                ctx.substitution_reason[0] ? ctx.substitution_reason : "reason not supplied");
    }

    print_formatted_answer(ctx.answer.data, ctx.stdout_is_tty);

    {
        json_object *assistant = json_object_new_object();
        if (assistant) {
            json_object_object_add(assistant, "role", json_object_new_string("assistant"));
            json_object_object_add(assistant, "content",
                                   json_object_new_string_len(ctx.answer.data,
                                                              (int)ctx.answer.len));
            json_object_array_add(messages, assistant);
        }
    }
    success = 1;

cleanup:
    if (headers) curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
    if (request) json_object_put(request);
    buffer_free(&ctx.pending);
    buffer_free(&ctx.raw_body);
    buffer_free(&ctx.answer);
    if (ctx.returned_files) json_object_put(ctx.returned_files);
    return success;
}

size_t ordinary_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct buffer *b = userdata;
    size_t total = size * nmemb;
    return buffer_append(b, ptr, total) ? total : 0;
}

json_object *fetch_model_catalog(void)
{
    CURL *curl = NULL;
    CURLcode result;
    struct buffer response = {0};
    json_object *root = NULL;
    json_object *source = NULL;
    json_object *catalog = NULL;
    long http_code = 0;
    size_t i;

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Could not initialize libcurl.\n");
        goto cleanup;
    }

    curl_easy_setopt(curl, CURLOPT_URL, API_BASE "/models");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ordinary_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "coarena-c-client/1.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (result != CURLE_OK) {
        fprintf(stderr, "Could not retrieve model catalogue: %s\n",
                curl_easy_strerror(result));
        goto cleanup;
    }
    if (http_code < 200 || http_code >= 300) {
        print_http_error(http_code, response.data);
        goto cleanup;
    }

    root = response.data ? json_tokener_parse(response.data) : NULL;
    if (!root) {
        fprintf(stderr, "Coarena returned invalid JSON for its model catalogue.\n");
        goto cleanup;
    }

    if (json_object_get_type(root) == json_type_array) {
        source = root;
    } else if (json_object_get_type(root) == json_type_object) {
        if (!json_object_object_get_ex(root, "data", &source) ||
            json_object_get_type(source) != json_type_array) {
            if (!json_object_object_get_ex(root, "models", &source) ||
                json_object_get_type(source) != json_type_array) {
                fprintf(stderr, "Unexpected model catalogue format.\n");
                goto cleanup;
            }
        }
    } else {
        fprintf(stderr, "Unexpected model catalogue format.\n");
        goto cleanup;
    }

    catalog = json_object_new_array();
    if (!catalog) goto cleanup;

    for (i = 0; i < json_object_array_length(source); ++i) {
        json_object *item = json_object_array_get_idx(source, i);
        json_object *id_obj = NULL;
        const char *id = NULL;

        if (json_object_get_type(item) == json_type_string) {
            id = json_object_get_string(item);
        } else if (json_object_get_type(item) == json_type_object &&
                   json_object_object_get_ex(item, "id", &id_obj) &&
                   json_object_get_type(id_obj) == json_type_string) {
            id = json_object_get_string(id_obj);
        }

        if (id && *id) {
            json_object_array_add(catalog, json_object_new_string(id));
        }
    }

    if (json_object_array_length(catalog) == 0) {
        fprintf(stderr, "Coarena's model catalogue contained no usable model IDs.\n");
        json_object_put(catalog);
        catalog = NULL;
    }

cleanup:
    if (root) json_object_put(root);
    if (curl) curl_easy_cleanup(curl);
    buffer_free(&response);
    return catalog;
}

void print_model_catalog(const json_object *catalog, const char *current_model)
{
    size_t i;
    size_t count = json_object_array_length(catalog);

    printf("\nModels currently offered by Coarena (%zu):\n", count);
    for (i = 0; i < count; ++i) {
        json_object *item = json_object_array_get_idx((json_object *)catalog, i);
        const char *id = json_object_get_string(item);
        printf("  %2zu. %s%s\n", i + 1, id,
               strcmp(id, current_model) == 0 ? "  [active]" : "");
    }
    puts("Select one with: /model <model-id-or-number>");
}

int change_model(const char *argument, char *current_model, size_t model_size)
{
    json_object *catalog;
    char *end = NULL;
    long index;
    size_t i;
    const char *chosen = NULL;

    catalog = fetch_model_catalog();
    if (!catalog) return 0;

    if (!argument || !*argument) {
        print_model_catalog(catalog, current_model);
        json_object_put(catalog);
        return 1;
    }

    errno = 0;
    index = strtol(argument, &end, 10);
    if (argument[0] && end && *end == '\0' && errno == 0) {
        if (index < 1 || (unsigned long)index > json_object_array_length(catalog)) {
            fprintf(stderr, "Model number is out of range. Use /models to see the list.\n");
            json_object_put(catalog);
            return 0;
        }
        chosen = json_object_get_string(
            json_object_array_get_idx(catalog, (size_t)index - 1));
    } else {
        for (i = 0; i < json_object_array_length(catalog); ++i) {
            const char *id = json_object_get_string(
                json_object_array_get_idx(catalog, i));
            if (strcmp(id, argument) == 0) {
                chosen = id;
                break;
            }
        }
    }

    if (!chosen) {
        fprintf(stderr, "Unknown model '%s'. Use /models to see available models.\n",
                argument);
        json_object_put(catalog);
        return 0;
    }

    if (snprintf(current_model, model_size, "%s", chosen) >= (int)model_size) {
        fprintf(stderr, "The model ID is too long for this client.\n");
        json_object_put(catalog);
        return 0;
    }

    printf("Active model changed to: %s\n", current_model);
    json_object_put(catalog);
    return 1;
}

int fetch_usage_info(const char *key, struct usage_info *usage, int quiet)
{
    CURL *curl = NULL;
    CURLcode result;
    struct curl_slist *headers = NULL;
    struct buffer response = {0};
    long http_code = 0;
    int success = 0;

    memset(usage, 0, sizeof(*usage));
    curl = curl_easy_init();
    if (!curl) {
        if (!quiet) fprintf(stderr, "Could not initialize libcurl for usage lookup.\n");
        goto cleanup;
    }
    headers = make_headers(key);
    if (!headers) {
        if (!quiet) fprintf(stderr, "Could not prepare usage request headers.\n");
        goto cleanup;
    }

    curl_easy_setopt(curl, CURLOPT_URL, API_BASE "/limits");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ordinary_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 40L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "coarena-c-client/1.2");

    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (result != CURLE_OK) {
        if (!quiet) fprintf(stderr, "Usage request failed: %s\n", curl_easy_strerror(result));
        goto cleanup;
    }
    if (http_code < 200 || http_code >= 300) {
        if (!quiet) print_http_error(http_code, response.data);
        goto cleanup;
    }

    {
        json_object *root = response.data ? json_tokener_parse(response.data) : NULL;
        json_object *data = NULL, *today = NULL, *left_obj = NULL, *reset_obj = NULL;
        if (!root ||
            !json_object_object_get_ex(root, "data", &data) ||
            !data || json_object_get_type(data) != json_type_object ||
            !json_object_object_get_ex(data, "today", &today) ||
            !today || json_object_get_type(today) != json_type_object ||
            !json_object_object_get_ex(today, "left_usd", &left_obj) ||
            !left_obj || (json_object_get_type(left_obj) != json_type_double &&
                          json_object_get_type(left_obj) != json_type_int)) {
            if (!quiet) fprintf(stderr, "Coarena returned an unexpected usage response.\n");
            if (root) json_object_put(root);
            goto cleanup;
        }

        usage->remaining_usd = json_object_get_double(left_obj);
        if (json_object_object_get_ex(today, "resets_at", &reset_obj) &&
            reset_obj && json_object_get_type(reset_obj) == json_type_int) {
            usage->resets_at_ms = json_object_get_int64(reset_obj);
            usage->have_reset = usage->resets_at_ms > 0;
        }
        json_object_put(root);
    }
    success = 1;

cleanup:
    if (headers) curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
    buffer_free(&response);
    return success;
}

void print_usage_info(const struct usage_info *usage)
{
    int color = isatty(STDOUT_FILENO);
    if (color) fputs("\033[1;32m", stdout);
    printf("$%.6f USD remaining", usage->remaining_usd);
    if (color) fputs("\033[0m", stdout);

    if (usage->have_reset) {
        time_t reset_time = (time_t)(usage->resets_at_ms / 1000);
        struct tm utc_tm;
        char reset_text[64];
        time_t now = time(NULL);
        long long seconds_left = (long long)reset_time - (long long)now;
        if (gmtime_r(&reset_time, &utc_tm) &&
            strftime(reset_text, sizeof(reset_text), "%Y-%m-%d %H:%M:%S UTC", &utc_tm)) {
            printf("  |  resets %s", reset_text);
            if (seconds_left > 0) {
                long long days = seconds_left / 86400;
                long long hours = (seconds_left % 86400) / 3600;
                long long minutes = (seconds_left % 3600) / 60;
                long long seconds = seconds_left % 60;
                if (days) printf(" (in %lldd %02lldh %02lldm)", days, hours, minutes);
                else if (hours) printf(" (in %lldh %02lldm)", hours, minutes);
                else if (minutes) printf(" (in %lldm %02llds)", minutes, seconds);
                else printf(" (in %llds)", seconds);
            } else {
                fputs(" (reset time reached)", stdout);
            }
        } else {
            fputs("  |  reset time unavailable", stdout);
        }
    } else {
        fputs("  |  reset time unavailable", stdout);
    }
    fputc('\n', stdout);
}

int select_api_key(struct api_keys *keys, struct usage_info *selected_usage)
{
    size_t i;
    size_t first_positive = (size_t)-1;
    struct usage_info first_positive_usage;

    if (!keys || keys->count == 0) return 0;
    if (keys->count == 1) {
        keys->selected = 0;
        if (selected_usage) memset(selected_usage, 0, sizeof(*selected_usage));
        return 1; /* A single key does not need balance-based routing. */
    }

    for (i = 0; i < keys->count; ++i) {
        struct usage_info usage;
        if (!fetch_usage_info(keys->items[i].value, &usage, 1)) {
            fprintf(stderr, "Warning: could not check the balance for API key %zu; skipping it.\n",
                    i + 1);
            continue;
        }

        if (usage.remaining_usd > KEY_SELECTION_THRESHOLD) {
            keys->selected = i;
            if (selected_usage) *selected_usage = usage;
            return 1;
        }
        if (usage.remaining_usd > 0.0 && first_positive == (size_t)-1) {
            first_positive = i;
            first_positive_usage = usage;
        }
    }

    if (first_positive != (size_t)-1) {
        keys->selected = first_positive;
        if (selected_usage) *selected_usage = first_positive_usage;
        return 1;
    }

    fprintf(stderr, "No API key has a verified positive USD balance; request not sent.\n");
    return 0;
}

int show_usage_for_keys(struct api_keys *keys)
{
    struct usage_info usage;
    if (!keys || keys->count == 0) return 0;

    if (keys->count > 1) {
        if (!select_api_key(keys, &usage)) return 0;
    } else {
        if (!fetch_usage_info(keys->items[0].value, &usage, 0)) return 0;
        keys->selected = 0;
    }
    print_usage_info(&usage);
    return 1;
}

