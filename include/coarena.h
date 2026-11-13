#ifndef COARENA_H
#define COARENA_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <curl/curl.h>
#include <json-c/json.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define API_BASE       "https://coarena.ai/api/v1"
#define MODEL_ID       "claude-fable-5-2"
#define MAX_IMAGE_SIZE (8U * 1024U * 1024U)
#define MAX_TEXT_SIZE  (2U * 1024U * 1024U)
#define MAX_RETURNED_FILE_SIZE (25U * 1024U * 1024U)
#define SESSION_SYSTEM_PROMPT "You are a helpful assistant. Answer clearly in Markdown. Use headings, bold and italic text, and properly indented lists when useful. When the user asks you to create or return a file, include every requested file in its own fenced block whose opening line is exactly file:filename.ext (for example, file:main.c), followed by the complete file contents and a closing fence. Use this file: format only for files intended to be saved, not for ordinary illustrative code snippets. Do not claim that a binary attachment was attached unless the API actually returned one."

#define MAX_API_KEYS 256
#define MAX_API_KEY_LENGTH 4095
#define KEY_SELECTION_THRESHOLD 0.20

struct buffer {
    char *data;
    size_t len;
    size_t cap;
};

struct stream_context {
    struct buffer pending;
    struct buffer raw_body;
    struct buffer answer;
    int api_error;
    int stdout_is_tty;
    char substituted_from[128];
    char substituted_to[128];
    char substitution_reason[128];
    char actual_model[128];
    json_object *returned_files;
};

struct api_key { char *value; };
struct api_keys { struct api_key *items; size_t count; size_t selected; };
struct usage_info { double remaining_usd; int64_t resets_at_ms; int have_reset; };
struct download_context { struct buffer bytes; int too_large; };

struct session_state { char id[128]; char path[PATH_MAX]; };
struct session_entry {
    char filename[NAME_MAX + 1];
    char id[128];
    char model[128];
    char title[160];
    time_t modified;
};

extern char app_dir[PATH_MAX];
extern char files_dir[PATH_MAX];
extern char sessions_dir[PATH_MAX];

/* Cross-module functions are declared here to keep module boundaries explicit. */
int make_directory(const char *path, mode_t mode);
int initialize_paths(void);
int format_timestamp(time_t when, char *out, size_t out_size);
int buffer_reserve(struct buffer *b, size_t extra);
int buffer_append(struct buffer *b, const void *data, size_t len);
void buffer_clear(struct buffer *b);
void buffer_free(struct buffer *b);
char *trim_whitespace(char *s);
void free_api_keys(struct api_keys *keys);
int load_api_keys(struct api_keys *keys);
int read_file(const char *path, size_t max_size, struct buffer *out);
const char *image_mime_type(const char *path);
char *base64_encode(const unsigned char *src, size_t len);
int parse_attachment_command(const char *line, const char *command, char **path_out, char **prompt_out);
json_object *make_user_message(const char *line);
void append_and_print(struct stream_context *ctx, const char *text, size_t len);
void handle_delta_content(struct stream_context *ctx, json_object *content);
void collect_file_fields(json_object *source, json_object *files);
void collect_typed_file_parts(json_object *content, json_object *files);
void process_sse_json(struct stream_context *ctx, const char *json_text);
void process_sse_line(struct stream_context *ctx, const char *line, size_t len);
size_t stream_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
void finish_pending_stream(struct stream_context *ctx);
size_t header_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
struct curl_slist *make_headers(const char *key);
void print_http_error(long code, const char *body);
const char *json_string_for_keys(json_object *obj, const char *const *keys, size_t count);
int write_all_fd(int fd, const unsigned char *data, size_t len);
void safe_filename(const char *suggested, char *out, size_t out_size);
int write_unique_file(const char *directory, const char *suggested, const unsigned char *data, size_t len, char *saved_name, size_t saved_name_size);
int base64_value(unsigned char c);
int base64_decode(const char *src, struct buffer *out);
size_t bounded_download_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
int download_https_file(const char *url, struct buffer *bytes);
int save_json_file_object(json_object *file_obj, const char *directory);
int save_structured_returned_files(json_object *files, const char *directory);
int line_is_file_fence(const char *line, size_t len, char *name, size_t name_size);
int line_is_closing_fence(const char *line, size_t len);
char *save_markdown_file_blocks(const char *answer, const char *directory);
void render_inline_markdown(const char *line, size_t len, int color);
void render_markdown(const char *text, int color);
void print_formatted_answer(const char *answer, int color);
int call_model(const char *key, json_object *messages, const char *model_id);
size_t ordinary_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
json_object *fetch_model_catalog(void);
void print_model_catalog(const json_object *catalog, const char *current_model);
int change_model(const char *argument, char *current_model, size_t model_size);
int fetch_usage_info(const char *key, struct usage_info *usage, int quiet);
void print_usage_info(const struct usage_info *usage);
int select_api_key(struct api_keys *keys, struct usage_info *selected_usage);
int show_usage_for_keys(struct api_keys *keys);
void print_help(void);
json_object *new_message(const char *role, const char *content);
void reset_history(json_object *messages);
int save_session(json_object *messages, const char *model, const struct session_state *session);
int create_session(json_object *messages, const char *model, struct session_state *session);
void extract_session_title(json_object *messages, char *title, size_t title_size);
int session_entry_compare(const void *a, const void *b);
struct session_entry *collect_sessions(size_t *count_out);
void list_sessions(void);
int open_session(const char *selector, json_object **messages, char *current_model, size_t model_size, struct session_state *session);
int main(void);

#endif /* COARENA_H */
