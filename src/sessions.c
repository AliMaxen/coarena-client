#include "coarena.h"

/* Persistent local conversation sessions. */
json_object *new_message(const char *role, const char *content)
{
    json_object *message = json_object_new_object();
    if (!message) return NULL;
    json_object_object_add(message, "role", json_object_new_string(role));
    json_object_object_add(message, "content", json_object_new_string(content));
    return message;
}

void reset_history(json_object *messages)
{
    while (json_object_array_length(messages) > 0) {
        json_object_array_del_idx(messages, json_object_array_length(messages) - 1, 1);
    }
    json_object_array_add(messages, new_message("system", SESSION_SYSTEM_PROMPT));
}

int save_session(json_object *messages, const char *model,
                        const struct session_state *session)
{
    json_object *root = NULL;
    char temp_path[PATH_MAX];
    const char *serialized;
    int fd = -1;
    int ok = 0;

    if (!messages || !session || !session->path[0]) return 0;
    root = json_object_new_object();
    if (!root) return 0;
    json_object_object_add(root, "version", json_object_new_int(1));
    json_object_object_add(root, "id", json_object_new_string(session->id));
    json_object_object_add(root, "model", json_object_new_string(model));
    json_object_object_add(root, "updated_at", json_object_new_int64((int64_t)time(NULL)));
    json_object_object_add(root, "messages", json_object_get(messages));
    serialized = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
    if (!serialized) goto cleanup;
    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld", session->path, (long)getpid())
        >= (int)sizeof(temp_path)) goto cleanup;

    fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "Could not save chat session: %s\n", strerror(errno));
        goto cleanup;
    }
    if (!write_all_fd(fd, (const unsigned char *)serialized, strlen(serialized)) ||
        fsync(fd) != 0) {
        fprintf(stderr, "Could not write chat session: %s\n", strerror(errno));
        close(fd);
        fd = -1;
        unlink(temp_path);
        goto cleanup;
    }
    if (close(fd) != 0) {
        fd = -1;
        unlink(temp_path);
        goto cleanup;
    }
    fd = -1;
    if (rename(temp_path, session->path) != 0) {
        fprintf(stderr, "Could not finalize chat session: %s\n", strerror(errno));
        unlink(temp_path);
        goto cleanup;
    }
    ok = 1;

cleanup:
    if (fd >= 0) close(fd);
    if (root) json_object_put(root);
    return ok;
}

int create_session(json_object *messages, const char *model,
                          struct session_state *session)
{
    time_t now = time(NULL);
    char stamp[32];
    unsigned int suffix;
    if (!format_timestamp(now, stamp, sizeof(stamp))) strcpy(stamp, "session");

    for (suffix = 0; suffix < 10000; ++suffix) {
        char filename[160];
        int n = snprintf(session->id, sizeof(session->id), "%s_%ld_%u",
                         stamp, (long)getpid(), suffix);
        if (n < 0 || (size_t)n >= sizeof(session->id)) return 0;
        n = snprintf(filename, sizeof(filename), "session_%s.json", session->id);
        if (n < 0 || (size_t)n >= sizeof(filename)) return 0;
        if (snprintf(session->path, sizeof(session->path), "%s/%s", sessions_dir, filename)
            >= (int)sizeof(session->path)) return 0;
        if (access(session->path, F_OK) != 0 && errno == ENOENT) break;
    }
    if (suffix == 10000) return 0;
    reset_history(messages);
    return save_session(messages, model, session);
}

void extract_session_title(json_object *messages, char *title, size_t title_size)
{
    size_t i;
    snprintf(title, title_size, "(empty chat)");
    if (!messages || json_object_get_type(messages) != json_type_array) return;
    for (i = 0; i < json_object_array_length(messages); ++i) {
        json_object *message = json_object_array_get_idx(messages, i);
        json_object *role = NULL, *content = NULL;
        const char *text = NULL;
        if (!json_object_object_get_ex(message, "role", &role) ||
            json_object_get_type(role) != json_type_string ||
            strcmp(json_object_get_string(role), "user") != 0) continue;
        if (!json_object_object_get_ex(message, "content", &content)) continue;
        if (json_object_get_type(content) == json_type_string) {
            text = json_object_get_string(content);
        } else if (json_object_get_type(content) == json_type_array &&
                   json_object_array_length(content) > 0) {
            json_object *part = json_object_array_get_idx(content, 0);
            json_object *part_text = NULL;
            if (json_object_object_get_ex(part, "text", &part_text) &&
                json_object_get_type(part_text) == json_type_string)
                text = json_object_get_string(part_text);
        }
        if (text) {
            size_t j, n = 0;
            for (j = 0; text[j] && n + 1 < title_size && n < 125; ++j) {
                unsigned char c = (unsigned char)text[j];
                if (isspace(c)) {
                    if (n && title[n - 1] != ' ') title[n++] = ' ';
                } else if (c >= 32 && c != 127) {
                    title[n++] = (char)c;
                }
            }
            while (n && title[n - 1] == ' ') --n;
            title[n] = '\0';
            if (!n) snprintf(title, title_size, "(empty prompt)");
            else if (text[j]) {
                if (n + 4 < title_size) strcat(title, "...");
            }
        }
        return;
    }
}

int session_entry_compare(const void *a, const void *b)
{
    const struct session_entry *left = a;
    const struct session_entry *right = b;
    if (left->modified < right->modified) return 1;
    if (left->modified > right->modified) return -1;
    return strcmp(right->id, left->id);
}

struct session_entry *collect_sessions(size_t *count_out)
{
    DIR *dir;
    struct dirent *entry;
    struct session_entry *items = NULL;
    size_t count = 0, capacity = 0;

    *count_out = 0;
    dir = opendir(sessions_dir);
    if (!dir) {
        fprintf(stderr, "Cannot open session directory: %s\n", strerror(errno));
        return NULL;
    }
    while ((entry = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        struct stat st;
        struct buffer json_data = {0};
        json_object *root = NULL, *id_obj = NULL, *model_obj = NULL, *messages = NULL;
        struct session_entry item;
        size_t name_len = strlen(entry->d_name);

        if (strncmp(entry->d_name, "session_", 8) != 0 || name_len < 13 ||
            strcmp(entry->d_name + name_len - 5, ".json") != 0) continue;
        if (snprintf(path, sizeof(path), "%s/%s", sessions_dir, entry->d_name)
            >= (int)sizeof(path)) continue;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        memset(&item, 0, sizeof(item));
        snprintf(item.filename, sizeof(item.filename), "%s", entry->d_name);
        item.modified = st.st_mtime;
        snprintf(item.id, sizeof(item.id), "%.*s",
                 (int)(name_len - 13), entry->d_name + 8);
        snprintf(item.model, sizeof(item.model), "unknown model");
        snprintf(item.title, sizeof(item.title), "(session metadata unavailable)");

        if (read_file(path, 64U * 1024U * 1024U, &json_data)) {
            root = json_data.data ? json_tokener_parse(json_data.data) : NULL;
            if (root) {
                if (json_object_object_get_ex(root, "id", &id_obj) &&
                    json_object_get_type(id_obj) == json_type_string)
                    snprintf(item.id, sizeof(item.id), "%s", json_object_get_string(id_obj));
                if (json_object_object_get_ex(root, "model", &model_obj) &&
                    json_object_get_type(model_obj) == json_type_string)
                    snprintf(item.model, sizeof(item.model), "%s", json_object_get_string(model_obj));
                if (json_object_object_get_ex(root, "messages", &messages))
                    extract_session_title(messages, item.title, sizeof(item.title));
            }
        }
        if (root) json_object_put(root);
        buffer_free(&json_data);

        if (count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 16;
            struct session_entry *grown = realloc(items, new_capacity * sizeof(*items));
            if (!grown) {
                fprintf(stderr, "Out of memory while listing sessions.\n");
                free(items);
                items = NULL;
                count = 0;
                break;
            }
            items = grown;
            capacity = new_capacity;
        }
        items[count++] = item;
    }
    closedir(dir);
    if (items && count > 1) qsort(items, count, sizeof(*items), session_entry_compare);
    *count_out = count;
    return items;
}

void list_sessions(void)
{
    size_t count = 0, i;
    struct session_entry *items = collect_sessions(&count);
    int color = isatty(STDOUT_FILENO);
    if (!items || count == 0) {
        puts("No saved chat sessions found.");
        free(items);
        return;
    }
    printf("\nSaved chats (%zu), newest first:\n", count);
    for (i = 0; i < count; ++i) {
        struct tm tm_value;
        char modified[32] = "unknown time";
        localtime_r(&items[i].modified, &tm_value);
        strftime(modified, sizeof(modified), "%Y-%m-%d %H:%M", &tm_value);
        if (color) fputs("\033[1;36m", stdout);
        printf("%3zu. %s", i + 1, items[i].id);
        if (color) fputs("\033[0m", stdout);
        printf("  |  %s  |  %s\n     %s\n", modified, items[i].model, items[i].title);
    }
    puts("Open one with: /open <number-or-id>");
    free(items);
}

int open_session(const char *selector, json_object **messages,
                        char *current_model, size_t model_size,
                        struct session_state *session)
{
    size_t count = 0, i, chosen = (size_t)-1;
    struct session_entry *items = collect_sessions(&count);
    char *end = NULL;
    long index;
    char path[PATH_MAX];
    struct buffer data = {0};
    json_object *root = NULL, *loaded_messages = NULL, *model_obj = NULL, *id_obj = NULL;

    if (!items || count == 0) {
        fprintf(stderr, "There are no saved sessions to open.\n");
        free(items);
        return 0;
    }
    errno = 0;
    index = strtol(selector, &end, 10);
    if (selector[0] && end && *end == '\0' && errno == 0) {
        if (index >= 1 && (unsigned long)index <= count) chosen = (size_t)index - 1;
    } else {
        for (i = 0; i < count; ++i) {
            if (strcmp(items[i].id, selector) == 0) {
                chosen = i;
                break;
            }
        }
    }
    if (chosen == (size_t)-1) {
        fprintf(stderr, "Session '%s' was not found. Use /history.\n", selector);
        free(items);
        return 0;
    }
    if (snprintf(path, sizeof(path), "%s/%s", sessions_dir, items[chosen].filename)
        >= (int)sizeof(path) || !read_file(path, 64U * 1024U * 1024U, &data)) {
        free(items);
        buffer_free(&data);
        return 0;
    }
    root = json_tokener_parse(data.data);
    buffer_free(&data);
    if (!root || !json_object_object_get_ex(root, "messages", &loaded_messages) ||
        json_object_get_type(loaded_messages) != json_type_array ||
        !json_object_object_get_ex(root, "model", &model_obj) ||
        json_object_get_type(model_obj) != json_type_string) {
        fprintf(stderr, "The selected session file is invalid or incomplete.\n");
        if (root) json_object_put(root);
        free(items);
        return 0;
    }
    if (snprintf(current_model, model_size, "%s", json_object_get_string(model_obj))
        >= (int)model_size) {
        fprintf(stderr, "The saved model ID is too long.\n");
        json_object_put(root);
        free(items);
        return 0;
    }
    if (snprintf(session->path, sizeof(session->path), "%s", path) >= (int)sizeof(session->path)) {
        json_object_put(root);
        free(items);
        return 0;
    }
    if (json_object_object_get_ex(root, "id", &id_obj) &&
        json_object_get_type(id_obj) == json_type_string)
        snprintf(session->id, sizeof(session->id), "%s", json_object_get_string(id_obj));
    else
        snprintf(session->id, sizeof(session->id), "%s", items[chosen].id);

    json_object_put(*messages);
    *messages = json_object_get(loaded_messages);
    json_object_put(root);
    printf("Opened session %s | model: %s\n", session->id, current_model);
    free(items);
    return 1;
}

