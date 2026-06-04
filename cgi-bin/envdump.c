#include <stdio.h>
#include <stdlib.h>

static const char *value_or_empty(const char *name) {
    const char *value = getenv(name);
    return value ? value : "";
}

int main(void) {
    const char *names[] = {
        "CONTENT_LENGTH",
        "CONTENT_TYPE",
        "GATEWAY_INTERFACE",
        "PATH_INFO",
        "QUERY_STRING",
        "REMOTE_ADDR",
        "REQUEST_METHOD",
        "REQUEST_URI",
        "SCRIPT_NAME",
        "SERVER_PORT",
        "SERVER_PROTOCOL",
        "SERVER_SOFTWARE",
        "HTTP_ACCEPT",
        "HTTP_REFERER",
        "HTTP_ACCEPT_ENCODING",
        "HTTP_ACCEPT_LANGUAGE",
        "HTTP_ACCEPT_CHARSET",
        "HTTP_HOST",
        "HTTP_COOKIE",
        "HTTP_USER_AGENT",
        "HTTP_CONNECTION"
    };
    size_t count = sizeof(names) / sizeof(names[0]);

    printf("Content-Type: text/plain; charset=utf-8\r\n\r\n");
    for (size_t i = 0; i < count; i++) {
        printf("%s=%s\n", names[i], value_or_empty(names[i]));
    }
    return 0;
}
