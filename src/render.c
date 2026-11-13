#include "coarena.h"

/* Small terminal-oriented Markdown renderer. */
void render_inline_markdown(const char *line, size_t len, int color)
{
    size_t i = 0;
    int bold = 0, italic = 0, code = 0;
    if (!color) {
        fwrite(line, 1, len, stdout);
        return;
    }
    while (i < len) {
        if (line[i] == '`') {
            code = !code;
            if (color) fputs(code ? "\033[38;5;114m" : "\033[0m", stdout);
            ++i;
            continue;
        }
        if (!code && i + 1 < len && line[i] == '*' && line[i + 1] == '*') {
            bold = !bold;
            if (color) {
                if (bold) fputs("\033[1m", stdout);
                else fputs("\033[22m", stdout);
            }
            i += 2;
            continue;
        }
        if (!code && line[i] == '*') {
            if (italic) {
                italic = 0;
                fputs("\033[23m", stdout);
                ++i;
                continue;
            }
            if (i + 1 < len && !isspace((unsigned char)line[i + 1])) {
                size_t close = i + 1;
                while (close < len && line[close] != '*') ++close;
                if (close < len) {
                    italic = 1;
                    fputs("\033[3m", stdout);
                    ++i;
                    continue;
                }
            }
        }
        if (!code && line[i] == '[') {
            size_t mid = i + 1, end;
            while (mid < len && line[mid] != ']') ++mid;
            if (mid + 1 < len && line[mid + 1] == '(') {
                end = mid + 2;
                while (end < len && line[end] != ')') ++end;
                if (end < len) {
                    if (color) fputs("\033[36m", stdout);
                    fwrite(line + i + 1, 1, mid - i - 1, stdout);
                    if (color) fputs("\033[0m\033[2m", stdout);
                    fputs(" (", stdout);
                    fwrite(line + mid + 2, 1, end - mid - 2, stdout);
                    fputc(')', stdout);
                    if (color) fputs("\033[0m", stdout);
                    i = end + 1;
                    continue;
                }
            }
        }
        fputc((unsigned char)line[i], stdout);
        ++i;
    }
    if (color) fputs("\033[0m", stdout);
}

void render_markdown(const char *text, int color)
{
    size_t pos = 0, total = strlen(text);
    int in_code = 0;
    while (pos < total) {
        size_t start = pos, end, content_start;
        size_t indent = 0;
        while (pos < total && text[pos] != '\n') ++pos;
        end = pos;
        if (pos < total) ++pos;
        content_start = start;
        while (content_start < end && (text[content_start] == ' ' || text[content_start] == '\t')) {
            indent += text[content_start] == '\t' ? 4 : 1;
            ++content_start;
        }
        if (end - content_start >= 3 && !memcmp(text + content_start, "```", 3)) {
            in_code = !in_code;
            if (in_code) {
                if (color) fputs("\033[2;36m", stdout);
                if (end > content_start + 3)
                    fwrite(text + content_start + 3, 1, end - content_start - 3, stdout);
                if (color) fputs("\033[0m", stdout);
                fputc('\n', stdout);
            } else {
                if (color) fputs("\033[0m", stdout);
                fputc('\n', stdout);
            }
            continue;
        }
        if (in_code) {
            if (color) fputs("\033[38;5;114m", stdout);
            fwrite(text + start, 1, end - start, stdout);
            if (color) fputs("\033[0m", stdout);
            fputc('\n', stdout);
            continue;
        }
        if (content_start < end && text[content_start] == '#') {
            size_t hashes = 0;
            while (content_start + hashes < end && text[content_start + hashes] == '#') ++hashes;
            if (content_start + hashes < end && text[content_start + hashes] == ' ') {
                content_start += hashes + 1;
                if (color) fputs("\033[1;36m", stdout);
                render_inline_markdown(text + content_start, end - content_start, color);
                if (color) fputs("\033[0m", stdout);
                fputc('\n', stdout);
                continue;
            }
        }
        if (content_start < end && text[content_start] == '>') {
            ++content_start;
            if (content_start < end && text[content_start] == ' ') ++content_start;
            if (color) fputs("\033[3;36m│ ", stdout);
            else fputs("| ", stdout);
            render_inline_markdown(text + content_start, end - content_start, color);
            fputc('\n', stdout);
            continue;
        }
        if (content_start + 1 < end &&
            ((text[content_start] == '-' || text[content_start] == '*' || text[content_start] == '+') &&
             text[content_start + 1] == ' ')) {
            size_t spaces = indent;
            while (spaces--) fputc(' ', stdout);
            if (color) fputs("\033[1;32m•\033[0m ", stdout);
            else fputs("• ", stdout);
            render_inline_markdown(text + content_start + 2, end - content_start - 2, color);
            fputc('\n', stdout);
            continue;
        }
        {
            size_t p = content_start;
            while (p < end && isdigit((unsigned char)text[p])) ++p;
            if (p > content_start && p + 1 < end && text[p] == '.' && text[p + 1] == ' ') {
                size_t spaces = indent;
                while (spaces--) fputc(' ', stdout);
                if (color) fputs("\033[1;32m", stdout);
                fwrite(text + content_start, 1, p - content_start + 1, stdout);
                if (color) fputs("\033[0m ", stdout);
                else fputc(' ', stdout);
                render_inline_markdown(text + p + 2, end - p - 2, color);
                fputc('\n', stdout);
                continue;
            }
        }
        render_inline_markdown(text + start, end - start, color);
        fputc('\n', stdout);
    }
}

