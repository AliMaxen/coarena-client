#include "coarena.h"

/* Filesystem paths, buffers, and common utility functions. */
char app_dir[PATH_MAX];
char files_dir[PATH_MAX];
char sessions_dir[PATH_MAX];

int make_directory(const char *path, mode_t mode)
{
    struct stat st;
    if (mkdir(path, mode) == 0) return 1;
    if (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (chmod(path, mode) != 0) {
            fprintf(stderr, "Cannot secure directory '%s': %s\n", path, strerror(errno));
            return 0;
        }
        return 1;
    }
    fprintf(stderr, "Cannot create directory '%s': %s\n", path, strerror(errno));
    return 0;
}

int initialize_paths(void)
{
    char executable[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (n > 0) {
        char *slash;
        executable[n] = '\0';
        slash = strrchr(executable, '/');
        if (slash) {
            *slash = '\0';
            if (snprintf(app_dir, sizeof(app_dir), "%s", executable) >= (int)sizeof(app_dir))
                return 0;
        }
    }
    if (!app_dir[0] && !getcwd(app_dir, sizeof(app_dir))) {
        fprintf(stderr, "Cannot determine the executable directory: %s\n", strerror(errno));
        return 0;
    }
    if (snprintf(files_dir, sizeof(files_dir), "%s/CoarenaFiles", app_dir) >= (int)sizeof(files_dir) ||
        snprintf(sessions_dir, sizeof(sessions_dir), "%s/CoarenaSessions", app_dir) >= (int)sizeof(sessions_dir)) {
        fprintf(stderr, "Executable directory path is too long.\n");
        return 0;
    }
    return make_directory(files_dir, 0700) && make_directory(sessions_dir, 0700);
}

int format_timestamp(time_t when, char *out, size_t out_size)
{
    struct tm tm_value;
    if (!localtime_r(&when, &tm_value)) return 0;
    return strftime(out, out_size, "%Y%m%d_%H%M%S", &tm_value) != 0;
}

int buffer_reserve(struct buffer *b, size_t extra)
{
    size_t needed;
    size_t new_cap;
    char *new_data;

    if (extra > SIZE_MAX - b->len - 1) {
        return 0;
    }

    needed = b->len + extra + 1;
    if (needed <= b->cap) {
        return 1;
    }

    new_cap = b->cap ? b->cap : 4096;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }

    new_data = realloc(b->data, new_cap);
    if (!new_data) {
        return 0;
    }

    b->data = new_data;
    b->cap = new_cap;
    if (b->len == 0) {
        b->data[0] = '\0';
    }
    return 1;
}

int buffer_append(struct buffer *b, const void *data, size_t len)
{
    if (!buffer_reserve(b, len)) {
        return 0;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
    return 1;
}

void buffer_clear(struct buffer *b)
{
    b->len = 0;
    if (b->data) {
        b->data[0] = '\0';
    }
}

void buffer_free(struct buffer *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

char *trim_whitespace(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s)) {
        ++s;
    }

    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
    return s;
}

int read_file(const char *path, size_t max_size, struct buffer *out)
{
    FILE *fp;
    unsigned char chunk[8192];
    size_t n;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Cannot open '%s': %s\n", path, strerror(errno));
        return 0;
    }

    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (n > max_size - out->len) {
            fprintf(stderr, "'%s' exceeds the %zu-byte attachment limit.\n",
                    path, max_size);
            fclose(fp);
            return 0;
        }
        if (!buffer_append(out, chunk, n)) {
            fprintf(stderr, "Out of memory while reading '%s'.\n", path);
            fclose(fp);
            return 0;
        }
    }

    if (ferror(fp)) {
        fprintf(stderr, "Error reading '%s'.\n", path);
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return 1;
}

