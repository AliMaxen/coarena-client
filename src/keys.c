#include "coarena.h"

/* API key loading and secure memory cleanup. */
void free_api_keys(struct api_keys *keys)
{
    size_t i;
    if (!keys) return;
    for (i = 0; i < keys->count; ++i) {
        if (keys->items[i].value) {
            volatile char *p = (volatile char *)keys->items[i].value;
            size_t n = strlen(keys->items[i].value);
            while (n--) *p++ = '\0';
            free(keys->items[i].value);
        }
    }
    free(keys->items);
    memset(keys, 0, sizeof(*keys));
}

int load_api_keys(struct api_keys *keys)
{
    char key_path[PATH_MAX];
    FILE *fp = NULL;
    struct stat st;
    char *line = NULL;
    size_t line_capacity = 0;
    ssize_t line_length;
    size_t allocated = 0;
    int ok = 0;

    memset(keys, 0, sizeof(*keys));
    if (snprintf(key_path, sizeof(key_path), "%s/key.txt", app_dir) >=
        (int)sizeof(key_path)) {
        fprintf(stderr, "The key.txt path is too long.\n");
        return 0;
    }

    fp = fopen(key_path, "r");
    if (!fp) {
        /* Preserve the previous current-directory fallback. */
        fp = fopen("key.txt", "r");
        if (fp) snprintf(key_path, sizeof(key_path), "./key.txt");
    }
    if (!fp) {
        fprintf(stderr,
                "Cannot open key.txt beside the executable or in the current directory.\n");
        return 0;
    }

    if (fstat(fileno(fp), &st) == 0 && (st.st_mode & (S_IRWXG | S_IRWXO))) {
        fprintf(stderr,
                "Warning: %s is accessible by group/others. Run: chmod 600 '%s'\n",
                key_path, key_path);
    }

    while ((line_length = getline(&line, &line_capacity, fp)) >= 0) {
        char *clean = trim_whitespace(line);
        size_t key_length;
        struct api_key *grown;

        (void)line_length;
        if (!*clean || *clean == '#') continue;
        key_length = strlen(clean);
        if (key_length > MAX_API_KEY_LENGTH) {
            fprintf(stderr, "An API key in key.txt exceeds %d characters.\n",
                    MAX_API_KEY_LENGTH);
            goto cleanup;
        }
        if (keys->count >= MAX_API_KEYS) {
            fprintf(stderr, "key.txt contains more than %d API keys.\n", MAX_API_KEYS);
            goto cleanup;
        }
        if (keys->count == allocated) {
            size_t next = allocated ? allocated * 2 : 8;
            if (next > MAX_API_KEYS) next = MAX_API_KEYS;
            grown = realloc(keys->items, next * sizeof(*keys->items));
            if (!grown) {
                fprintf(stderr, "Out of memory while loading API keys.\n");
                goto cleanup;
            }
            keys->items = grown;
            allocated = next;
        }
        keys->items[keys->count].value = strdup(clean);
        if (!keys->items[keys->count].value) {
            fprintf(stderr, "Out of memory while copying an API key.\n");
            goto cleanup;
        }
        ++keys->count;
    }

    if (ferror(fp)) {
        fprintf(stderr, "Error reading %s.\n", key_path);
        goto cleanup;
    }
    if (keys->count == 0) {
        fprintf(stderr, "key.txt contains no API keys.\n");
        goto cleanup;
    }

    keys->selected = 0;
    ok = 1;

cleanup:
    if (fp) fclose(fp);
    if (line) {
        volatile char *wipe = (volatile char *)line;
        size_t n = strlen(line);
        while (n--) *wipe++ = '\0';
        free(line);
    }
    if (!ok) free_api_keys(keys);
    return ok;
}

