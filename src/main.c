#include "coarena.h"

/* Interactive command loop. */
void print_help(void)
{
    puts("\nCommands:\n"
         "  /usg                        Show selected key's remaining USD and UTC reset time\n"
         "  /models                     Fetch and list Coarena's current model catalogue\n"
         "  /model <id-or-number>       Change the active model (also /model to list)\n"
         "  /new                        Save this chat and start a fresh session\n"
         "  /history                    List locally saved chat sessions\n"
         "  /open <number-or-id>        Open a saved session from /history\n"
         "  /image \"path\" | prompt     Send one local image with a question\n"
         "  /file  \"path\" | prompt     Include a local text file with a question\n"
         "  /reset                      Clear the current session's conversation\n"
         "  /help                       Show this help\n"
         "  /exit or /quit              Exit\n\n"
         "Any other line is sent as a normal prompt.\n"
         "Image types: JPG, JPEG, PNG, GIF, WEBP. Max image size: 8 MiB.\n"
         "Text attachments: plain text/source/config/log/CSV/Markdown, max 2 MiB.\n"
         "Generated files marked as file:filename.ext are saved in CoarenaFiles/.\n"
         "Chats are saved locally in CoarenaSessions/.\n"
         "key.txt: one API key per line; blank lines and # comments are ignored.\n"
         "With multiple keys, the first key above $0.20 is preferred; if none is above\n"
         "$0.20, the first key with any positive balance is used.\n");
}

int main(void)
{
    struct api_keys keys;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    json_object *messages = NULL;
    char current_model[128] = MODEL_ID;
    struct session_state session;

    memset(&session, 0, sizeof(session));
    memset(&keys, 0, sizeof(keys));
    if (!initialize_paths()) return 1;
    if (!load_api_keys(&keys)) return 1;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "Could not initialize libcurl.\n");
        free_api_keys(&keys);
        return 1;
    }

    messages = json_object_new_array();
    if (!messages) {
        fprintf(stderr, "Could not allocate conversation history.\n");
        curl_global_cleanup();
        free_api_keys(&keys);
        return 1;
    }
    if (!create_session(messages, current_model, &session)) {
        fprintf(stderr, "Could not create a local chat session.\n");
        json_object_put(messages);
        curl_global_cleanup();
        free_api_keys(&keys);
        return 1;
    }

    printf("Coarena C client | model: %s\n", current_model);
    printf("Loaded %zu API key%s.\n", keys.count, keys.count == 1 ? "" : "s");
    if (keys.count > 1)
        puts("Multi-key routing: prefer first balance > $0.20; otherwise drain positive balances in file order.");
    printf("Session: %s\n", session.id);
    puts("Type /help for commands. Type /exit or /quit to leave.");

    for (;;) {
        json_object *user_message;
        char *clean;

        fputs("\nYou> ", stdout);
        fflush(stdout);
        line_len = getline(&line, &line_cap, stdin);
        if (line_len < 0) {
            putchar('\n');
            break;
        }
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line[--line_len] = '\0';
        }
        clean = trim_whitespace(line);
        if (clean != line) memmove(line, clean, strlen(clean) + 1);

        if (!line[0]) continue;
        if (!strcasecmp(line, "/exit") || !strcasecmp(line, "/quit")) break;
        if (!strcasecmp(line, "/usg")) {
            show_usage_for_keys(&keys);
            continue;
        }
        if (!strcasecmp(line, "/models")) {
            json_object *catalog = fetch_model_catalog();
            if (catalog) {
                print_model_catalog(catalog, current_model);
                json_object_put(catalog);
            }
            continue;
        }
        if (!strncasecmp(line, "/model", 6) &&
            (line[6] == '\0' || isspace((unsigned char)line[6]))) {
            char *argument = trim_whitespace(line + 6);
            if (change_model(argument, current_model, sizeof(current_model)))
                save_session(messages, current_model, &session);
            continue;
        }
        if (!strcasecmp(line, "/new")) {
            save_session(messages, current_model, &session);
            if (create_session(messages, current_model, &session))
                printf("Started new chat session: %s\n", session.id);
            else
                fprintf(stderr, "Could not create a new chat session.\n");
            continue;
        }
        if (!strcasecmp(line, "/history")) {
            list_sessions();
            continue;
        }
        if (!strncasecmp(line, "/open", 5) &&
            (line[5] == '\0' || isspace((unsigned char)line[5]))) {
            char *selector = trim_whitespace(line + 5);
            if (!*selector) {
                fprintf(stderr, "Usage: /open <number-or-id>\n");
            } else {
                save_session(messages, current_model, &session);
                open_session(selector, &messages, current_model, sizeof(current_model), &session);
            }
            continue;
        }
        if (!strcasecmp(line, "/help")) {
            print_help();
            continue;
        }
        if (!strcasecmp(line, "/reset")) {
            reset_history(messages);
            save_session(messages, current_model, &session);
            puts("Current conversation cleared.");
            continue;
        }

        user_message = make_user_message(line);
        if (!user_message) {
            /* Attachment command was invalid or could not be read. */
            if (!strncmp(line, "/image", 6) || !strncmp(line, "/file", 5)) continue;
            fprintf(stderr, "Could not create the request message.\n");
            continue;
        }
        json_object_array_add(messages, user_message);
        save_session(messages, current_model, &session);

        {
            struct usage_info selected_usage;
            if (!select_api_key(&keys, &selected_usage)) {
                if (json_object_array_length(messages) > 1)
                    json_object_array_del_idx(messages, json_object_array_length(messages) - 1, 1);
                save_session(messages, current_model, &session);
                continue;
            }
            if (keys.count > 1) {
                if (isatty(STDOUT_FILENO)) fputs("\033[1;36m", stdout);
                printf("[Using API key %zu/%zu; $%.6f remaining]",
                       keys.selected + 1, keys.count, selected_usage.remaining_usd);
                if (isatty(STDOUT_FILENO)) fputs("\033[0m", stdout);
                fputc('\n', stdout);
            }
        }
        if (!call_model(keys.items[keys.selected].value, messages, current_model)) {
            /* Do not keep a prompt in history when its request failed. */
            if (json_object_array_length(messages) > 1) {
                json_object_array_del_idx(messages, json_object_array_length(messages) - 1, 1);
            }
        }
        save_session(messages, current_model, &session);
    }

    save_session(messages, current_model, &session);
    free(line);
    json_object_put(messages);
    free_api_keys(&keys);
    curl_global_cleanup();
    return 0;
}

