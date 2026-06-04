#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src) {
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) &&
                   isxdigit((unsigned char)src[2])) {
            int hi = hex_value(src[1]);
            int lo = hex_value(src[2]);
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static void html_escape_print(const char *s) {
    for (; s && *s; s++) {
        switch (*s) {
            case '&': fputs("&amp;", stdout); break;
            case '<': fputs("&lt;", stdout); break;
            case '>': fputs("&gt;", stdout); break;
            case '"': fputs("&quot;", stdout); break;
            case '\'': fputs("&#39;", stdout); break;
            default: fputc(*s, stdout); break;
        }
    }
}

static void extract_field(const char *data, const char *name, char *out, size_t out_len) {
    size_t name_len = strlen(name);
    const char *p = data;

    if (out_len == 0) return;
    out[0] = '\0';
    while (p && *p) {
        const char *end = strchr(p, '&');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > name_len && strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            char encoded[1024];
            size_t value_len = len - name_len - 1;
            if (value_len >= sizeof(encoded)) value_len = sizeof(encoded) - 1;
            memcpy(encoded, p + name_len + 1, value_len);
            encoded[value_len] = '\0';
            url_decode(out, encoded);
            out[out_len - 1] = '\0';
            return;
        }
        p = end ? end + 1 : NULL;
    }
}

int main(void) {
    const char *method = getenv("REQUEST_METHOD");
    const char *query = getenv("QUERY_STRING");
    const char *content_length_env = getenv("CONTENT_LENGTH");
    char data[4096] = {0};
    char username[512] = {0};
    char password[512] = {0};

    if (method && strcmp(method, "POST") == 0) {
        long content_length = content_length_env ? strtol(content_length_env, NULL, 10) : 0;
        if (content_length < 0) content_length = 0;
        if (content_length >= (long)sizeof(data)) content_length = (long)sizeof(data) - 1;
        if (content_length > 0) {
            size_t got = fread(data, 1, (size_t)content_length, stdin);
            data[got] = '\0';
        }
    } else if (query) {
        strncpy(data, query, sizeof(data) - 1);
    }

    extract_field(data, "username", username, sizeof(username));
    extract_field(data, "password", password, sizeof(password));

    printf("Content-Type: text/html; charset=utf-8\r\n\r\n");
    printf("<!DOCTYPE html>\n<html>\n<head><meta charset=\"UTF-8\"><title>CGI Result</title></head>\n<body>\n");
    printf("<h1>CGI Login Result</h1>\n");
    printf("<p>Username: ");
    html_escape_print(username);
    printf("</p>\n<p>Password: ");
    html_escape_print(password);
    printf("</p>\n</body>\n</html>\n");
    return 0;
}
