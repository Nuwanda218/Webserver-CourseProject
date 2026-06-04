#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>    /* 文件状态检测 */
#include <fcntl.h>       /* 文件操作 */
#include <errno.h>       /* 错误处理 */
#include <time.h>        /* 日志时间 */
#include <stdarg.h>      /* 可变参数 */
#include <ctype.h>       /* 字符处理 */
#include <limits.h>
#include <sys/wait.h>

#define ECHO_PORT 9999           /* echo服务器监听端口 */
#define BUF_SIZE 8192            /* 缓冲区大小 */
#define MAX_HEADER_SIZE 8192     /* 头部最大字节数 */
#define READ_CHUNK_SIZE 4096     /* socket读取块大小 */
#define MAX_CLIENTS  FD_SETSIZE   // 通常为 1024



/* HTTP响应消息定义 */
#define RESPONSE_200 "HTTP/1.1 200 OK\r\n"
#define RESPONSE_400 "HTTP/1.1 400 Bad request\r\n\r\n"
#define RESPONSE_404 "HTTP/1.1 404 Not Found\r\n\r\n"
#define RESPONSE_501 "HTTP/1.1 501 Not Implemented\r\n\r\n"
#define RESPONSE_505 "HTTP/1.1 505 HTTP Version not supported\r\n\r\n"
#define RESPONSE_500 "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n"
#define CGI_TIMEOUT_SEC 5
#define CGI_MAX_OUTPUT (1024 * 1024)

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} ConnectionBuffer;

typedef struct {
    int sock;                     // 客户端 socket，-1 表示空闲
    ConnectionBuffer buffer;      // 该连接未处理完的数据
    char ip[INET_ADDRSTRLEN];     // 客户端 IP
    int keep_alive;               // 期望保持连接（根据 Connection 头部）
    // 可扩展其他状态
} Client;

static Client clients[MAX_CLIENTS];
static fd_set master_set, read_set;
static int max_fd;                // 当前最大的文件描述符值

/* 请求解析状态 */
typedef enum {
    REQ_VALID,          /* 支持的GET/HEAD/POST方法，格式正确 */
    REQ_NOT_IMPL,       /* 方法不支持（非GET/HEAD/POST） */
    REQ_BAD,            /* 格式错误 */
    REQ_VERSION_ERR     /* HTTP版本不支持 */
} req_status;

/* 哈希表条目结构 */
typedef struct HashMapEntry {
    char *key;                 /* 键 */
    char *value;               /* 值 */
    struct HashMapEntry *next; /* 链表下一个节点 */
} HashMapEntry;

/* 哈希表结构 */
typedef struct {
    HashMapEntry *buckets[256]; /* 256个桶 */
} HashMap;

/* 日志文件指针 */
static FILE *access_log = NULL; /* 访问日志 */
static FILE *error_log = NULL;  /* 错误日志 */




/* 关闭socket的辅助函数 */
int close_socket(int sock) {
    if (close(sock)) {
        fprintf(stderr, "Failed closing socket.\n");
        return 1;
    }
    return 0;
}

/* 忽略SIGPIPE信号，防止写已关闭连接时崩溃 */
void handle_sigpipe(const int sig) {
    (void)sig;  /* 避免未使用参数警告 */
}

/*
 * 哈希函数：DJB算法
 * 参数:
 *   key: 字符串键
 * 返回值:
 *   哈希值（0-255）
 */
static unsigned int hash_str(const char *key) {
    unsigned int hash = 0;
    while (*key) {
        hash = (hash << 5) + tolower((unsigned char)*key++);
    }
    return hash % 256;
}

/*
 * 创建哈希表
 * 返回值:
 *   哈希表指针
 */
static HashMap *hashmap_create(void) {
    HashMap *map = (HashMap *)malloc(sizeof(HashMap));
    memset(map, 0, sizeof(HashMap));
    return map;
}

/*
 * 销毁哈希表
 * 参数:
 *   map: 哈希表指针
 */
static void hashmap_destroy(HashMap *map) {
    if (!map) return;
    for (int i = 0; i < 256; i++) {
        HashMapEntry *entry = map->buckets[i];
        while (entry) {
            HashMapEntry *next = entry->next;
            free(entry->key);
            free(entry->value);
            free(entry);
            entry = next;
        }
    }
    free(map);
}

/*
 * 插入或更新键值对
 * 参数:
 *   map: 哈希表指针
 *   key: 键
 *   value: 值
 */
static void hashmap_put(HashMap *map, const char *key, const char *value) {
    if (!map || !key || !value) return;
    unsigned int h = hash_str(key);
    HashMapEntry *entry = map->buckets[h];
    while (entry) {
        if (strcasecmp(entry->key, key) == 0) {
            free(entry->value);
            entry->value = strdup(value);
            return;
        }
        entry = entry->next;
    }
    entry = (HashMapEntry *)malloc(sizeof(HashMapEntry));
    entry->key = strdup(key);
    entry->value = strdup(value);
    entry->next = map->buckets[h];
    map->buckets[h] = entry;
}

/*
 * 根据键查找值
 * 参数:
 *   map: 哈希表指针
 *   key: 键
 * 返回值:
 *   值（不存在返回NULL）
 */
static const char *hashmap_get(HashMap *map, const char *key) {
    if (!map || !key) return NULL;
    unsigned int h = hash_str(key);
    HashMapEntry *entry = map->buckets[h];
    while (entry) {
        if (strcasecmp(entry->key, key) == 0) {
            return entry->value;
        }
        entry = entry->next;
    }
    return NULL;
}

/*
 * 获取Apache格式时间
 * 参数:
 *   buf: 时间字符串缓冲区
 *   buf_len: 缓冲区长度
 */
static void get_apache_time(char *buf, size_t buf_len) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, buf_len, "%d/%b/%Y:%H:%M:%S %z", tm);
}

/*
 * 初始化日志系统
 * 参数:
 *   access_path: 访问日志文件路径
 *   error_path: 错误日志文件路径
 */
static int logger_init(const char *access_path, const char *error_path) {
    access_log = fopen(access_path, "a");
    if (!access_log) {
        fprintf(stderr, "Failed to open access log: %s\n", access_path);
        return -1;
    }
    error_log = fopen(error_path, "a");
    if (!error_log) {
        fprintf(stderr, "Failed to open error log: %s\n", error_path);
        fclose(access_log);
        access_log = NULL;
        return -1;
    }
    return 0;
}

/*
 * 关闭日志系统
 */
static void logger_close(void) {
    if (access_log) fclose(access_log);
    if (error_log) fclose(error_log);
}

/*
 * 记录访问日志（Apache Common Log Format）
 * 参数:
 *   client_ip: 客户端IP
 *   method: HTTP方法
 *   uri: 请求URI
 *   version: HTTP版本
 *   status: 响应状态码
 *   bytes: 响应字节数
 */
static void logger_access(const char *client_ip, const char *method, const char *uri,
                          const char *version, int status, long long bytes) {
    if (!access_log) return;
    char time_str[64];
    get_apache_time(time_str, sizeof(time_str));
    fprintf(access_log, "%s - - [%s] \"%s %s %s\" %d %lld\n",
            client_ip, time_str, method, uri, version, status, bytes);
    fflush(access_log);
}

/*
 * 记录错误日志（Apache Error Log格式）
 * 参数:
 *   level: 日志级别（error/warn/info/debug）
 *   msg: 错误消息
 */
static void logger_error(const char *level, const char *msg) {
    if (!error_log) return;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%a %b %d %H:%M:%S %Y", tm);
    fprintf(error_log, "[%s] [%s] %s\n", time_str, level, msg);
    fflush(error_log);
}

/* MIME类型映射表 */
static const char *mime_types[][2] = {
    {".html", "text/html"}, {".htm", "text/html"}, {".css", "text/css"},
    {".js", "application/javascript"}, {".png", "image/png"},
    {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
    {".txt", "text/plain"}, {NULL, NULL}
};

/*
 * 获取文件的MIME类型
 * 参数:
 *   path: 文件路径
 * 返回值:
 *   MIME类型字符串
 */
static const char *get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    for (int i = 0; mime_types[i][0]; i++) {
        if (strcasecmp(ext, mime_types[i][0]) == 0) {
            return mime_types[i][1];
        }
    }
    return "application/octet-stream";
}

/*
 * 将URI解析为静态文件路径。拒绝路径越界和过长路径。
 */
static int resolve_path(const char *uri, char *path, size_t path_len) {
    const char *static_dir = "./static_site";
    const char *resource = uri;
    char uri_copy[4096];
    char *query = NULL;
    int written;

    if (!uri || uri[0] != '/' || strstr(uri, "..")) {
        return 0;
    }

    written = snprintf(uri_copy, sizeof(uri_copy), "%s", uri);
    if (written < 0 || (size_t)written >= sizeof(uri_copy)) {
        return 0;
    }

    query = strchr(uri_copy, '?');
    if (query) *query = '\0';
    query = strchr(uri_copy, '#');
    if (query) *query = '\0';

    if (strcmp(uri_copy, "/") == 0) {
        resource = "/index.html";
    } else {
        resource = uri_copy;
    }

    written = snprintf(path, path_len, "%s%s", static_dir, resource);
    return written >= 0 && (size_t)written < path_len;
}

static void log_file_error(const char *path, const char *what) {
    char msg[1024];
    snprintf(msg, sizeof(msg), "%s: %s: %s", what, path ? path : "-", strerror(errno));
    logger_error("error", msg);
}

/*
 * 完整发送数据
 * 参数:
 *   sock: socket描述符
 *   buf: 数据缓冲区
 *   len: 数据长度
 * 返回值:
 *   0: 成功，-1: 失败
 */
static int send_all(int sock, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += n;
    }
    return 0;
}

/*
 * 发送文件响应
 * 参数:
 *   sock: socket描述符
 *   file_path: 文件路径
 *   method: HTTP方法（HEAD不发送body）
 * 返回值:
 *   HTTP状态码（200/404），-1表示发送失败
 */
static int send_file_response(int sock, const char *file_path, const char *method,
                              int keep_alive, long long *body_bytes) {
    struct stat st;
    int fd;
    const char *mime;
    char header[BUF_SIZE];
    int header_len;
    char buf[READ_CHUNK_SIZE];
    ssize_t r;

    if (stat(file_path, &st) != 0) {
        log_file_error(file_path, "stat failed");
        send_all(sock, RESPONSE_404, strlen(RESPONSE_404));
        return 404;
    }

    if (!S_ISREG(st.st_mode)) {
        logger_error("error", "requested path is not a regular file");
        send_all(sock, RESPONSE_404, strlen(RESPONSE_404));
        return 404;
    }

    fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        log_file_error(file_path, "open failed");
        send_all(sock, RESPONSE_404, strlen(RESPONSE_404));
        return 404;
    }

    mime = get_mime_type(file_path);
    if (body_bytes) *body_bytes = (long long)st.st_size;
    header_len = snprintf(header, sizeof(header),
        "%sContent-Type: %s\r\nContent-Length: %lld\r\nConnection: %s\r\n\r\n",
        RESPONSE_200, mime, (long long)st.st_size,
        keep_alive ? "keep-alive" : "close");
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        close(fd);
        logger_error("error", "response header too large");
        return -1;
    }

    if (send_all(sock, header, (size_t)header_len) < 0) {
        close(fd);
        return -1;
    }

    if (strcmp(method, "HEAD") == 0) {
        close(fd);
        return 200;
    }

    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        if (send_all(sock, buf, (size_t)r) < 0) {
            close(fd);
            logger_error("error", "Failed to send file content");
            return -1;
        }
    }
    if (r < 0) {
        log_file_error(file_path, "read failed");
        close(fd);
        return -1;
    }
    close(fd);
    return 200;
}

static int send_post_raw_echo_response(int sock, const char *request_raw, size_t raw_len) {
    char header[BUF_SIZE];
    int header_len = snprintf(header, sizeof(header),
        "%sContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
        RESPONSE_200, raw_len);

    if (header_len < 0 || (size_t)header_len >= sizeof(header)) return -1;
    if (send_all(sock, header, (size_t)header_len) < 0) return -1;
    if (raw_len > 0 && send_all(sock, request_raw, raw_len) < 0) return -1;
    return 200;
}

/*
 * 判断是否保持持久连接
 * 参数:
 *   headers: HTTP头部哈希表
 * 返回值:
 *   1: 保持连接，0: 关闭连接
 */
static int keep_alive(HashMap *headers) {
    const char *conn = hashmap_get(headers, "Connection");
    if (conn && strcasecmp(conn, "close") == 0) return 0;
    return 1;
}

/*
 * 解析HTTP请求原始报文（仅解析头部，不处理body）
 * 参数:
 *   raw: 原始请求字符串（以'\0'结尾）
 *   method: 输出方法名
 *   uri: 输出URI
 *   version: 输出HTTP版本
 *   headers: 输出头部哈希表
 * 返回值:
 *   REQ_VALID   : 方法为GET/HEAD/POST，且格式正确
 *   REQ_NOT_IMPL: 方法为其他（如PUT/DELETE等），且请求行格式基本正确
 *   REQ_BAD     : 格式错误（包括空请求、请求行错误、头部错误等）
 *   REQ_VERSION_ERR: HTTP版本不支持
 */
static int method_is_alpha(const char *method) {
    if (!method || method[0] == '\0') return 0;
    for (const char *m = method; *m; m++) {
        if (!isalpha((unsigned char)*m)) return 0;
    }
    return 1;
}

static int header_name_is_valid(const char *name) {
    if (!name || name[0] == '\0') return 0;
    for (const char *p = name; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch <= 32 || ch == 127 || ch == ':') return 0;
    }
    return 1;
}

static int http_version_is_valid(const char *version) {
    const char *major;
    const char *dot;
    const char *minor;

    if (!version || strncmp(version, "HTTP/", 5) != 0) return 0;
    major = version + 5;
    dot = strchr(major, '.');
    if (!dot || dot == major || dot[1] == '\0') return 0;
    for (const char *p = major; p < dot; p++) {
        if (!isdigit((unsigned char)*p)) return 0;
    }
    minor = dot + 1;
    for (const char *p = minor; *p; p++) {
        if (!isdigit((unsigned char)*p)) return 0;
    }
    return 1;
}

static void trim_trailing_ows(char *value) {
    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t')) {
        value[--len] = '\0';
    }
}

static req_status parse_http_request(const char *raw,
                                     char *method, char *uri, char *version,
                                     HashMap *headers) {
    char *buf;
    char *request_line_end;
    char *header_start;
    char *empty_line;
    char *method_start;
    char *uri_start;
    char *version_start;
    char *sp;
    char *sp2;
    size_t line_delim_len = 2;

    if (raw == NULL || raw[0] == '\0') return REQ_BAD;

    buf = strdup(raw);
    if (!buf) return REQ_BAD;

    request_line_end = strstr(buf, "\r\n");
    if (!request_line_end) {
        request_line_end = strchr(buf, '\n');
        line_delim_len = 1;
    }
    if (!request_line_end) {
        free(buf);
        return REQ_BAD;
    }
    *request_line_end = '\0';

    method_start = buf;
    if (method_start[0] == ' ' || method_start[0] == '\t') {
        free(buf);
        return REQ_BAD;
    }

    sp = strchr(method_start, ' ');
    if (!sp) {
        free(buf);
        return REQ_BAD;
    }
    *sp = '\0';

    uri_start = sp + 1;
    sp2 = strchr(uri_start, ' ');
    if (!sp2) {
        free(buf);
        return REQ_BAD;
    }
    *sp2 = '\0';
    version_start = sp2 + 1;

    if (method_start[0] == '\0' || uri_start[0] == '\0' || version_start[0] == '\0' ||
        strchr(version_start, ' ') || strchr(version_start, '\t') ||
        !method_is_alpha(method_start) || !http_version_is_valid(version_start) ||
        strlen(method_start) >= 64 || strlen(uri_start) >= 256 || strlen(version_start) >= 64) {
        free(buf);
        return REQ_BAD;
    }

    header_start = request_line_end + line_delim_len;
    empty_line = strstr(header_start, "\r\n\r\n");
    if (!empty_line) {
        empty_line = strstr(header_start, "\n\n");
    }
    if (!empty_line) {
        free(buf);
        return REQ_BAD;
    }

    for (char *line = header_start; line < empty_line; ) {
        char *line_end = strstr(line, "\r\n");
        size_t delim_len = 2;
        char *colon;
        char *value;

        if (!line_end || line_end > empty_line) {
            line_end = strchr(line, '\n');
            delim_len = 1;
        }
        if (!line_end || line_end > empty_line) {
            free(buf);
            return REQ_BAD;
        }

        *line_end = '\0';
        colon = strchr(line, ':');
        if (!colon || colon == line) {
            free(buf);
            return REQ_BAD;
        }
        *colon = '\0';
        if (!header_name_is_valid(line)) {
            free(buf);
            return REQ_BAD;
        }

        value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        trim_trailing_ows(value);
        hashmap_put(headers, line, value);
        line = line_end + delim_len;
    }

    strcpy(method, method_start);
    strcpy(uri, uri_start);
    strcpy(version, version_start);
    free(buf);

    if (strcmp(method, "GET") == 0 ||
        strcmp(method, "HEAD") == 0 ||
        strcmp(method, "POST") == 0) {
        if (strcmp(version, "HTTP/1.1") != 0) return REQ_VERSION_ERR;
        return REQ_VALID;
    }
    return REQ_NOT_IMPL;
}

static void buffer_init(ConnectionBuffer *buffer) {
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static void buffer_free(ConnectionBuffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static int buffer_reserve(ConnectionBuffer *buffer, size_t needed) {
    char *new_data;
    size_t new_cap = buffer->cap ? buffer->cap : BUF_SIZE;

    if (needed <= buffer->cap) return 0;
    while (new_cap < needed) {
        if (new_cap > ((size_t)-1) / 2) return -1;
        new_cap *= 2;
    }

    new_data = realloc(buffer->data, new_cap);
    if (!new_data) return -1;
    buffer->data = new_data;
    buffer->cap = new_cap;
    return 0;
}

static void buffer_consume(ConnectionBuffer *buffer, size_t count) {
    if (count >= buffer->len) {
        buffer->len = 0;
        if (buffer->data) buffer->data[0] = '\0';
        return;
    }
    memmove(buffer->data, buffer->data + count, buffer->len - count);
    buffer->len -= count;
    buffer->data[buffer->len] = '\0';
}

static ssize_t buffer_recv(int sock, ConnectionBuffer *buffer) {
    char chunk[READ_CHUNK_SIZE];
    ssize_t n;

    do {
        n = recv(sock, chunk, sizeof(chunk), 0);
    } while (n < 0 && errno == EINTR);

    if (n <= 0) return n;
    if (buffer_reserve(buffer, buffer->len + (size_t)n + 1) < 0) return -1;
    memcpy(buffer->data + buffer->len, chunk, (size_t)n);
    buffer->len += (size_t)n;
    buffer->data[buffer->len] = '\0';
    return n;
}

static int find_header_end(ConnectionBuffer *buffer, size_t *header_len) {
    char *end;

    if (!buffer->data) return 0;
    end = strstr(buffer->data, "\r\n\r\n");
    if (end) {
        *header_len = (size_t)(end - buffer->data) + 4;
        return 1;
    }
    end = strstr(buffer->data, "\n\n");
    if (end) {
        *header_len = (size_t)(end - buffer->data) + 2;
        return 1;
    }
    return 0;
}

static int read_complete_header(int sock, ConnectionBuffer *buffer, size_t *header_len) {
    while (!find_header_end(buffer, header_len)) {
        ssize_t n;
        if (buffer->len > MAX_HEADER_SIZE) return -2;
        n = buffer_recv(sock, buffer);
        if (n == 0) return 0;
        if (n < 0) return -1;
    }
    return *header_len > MAX_HEADER_SIZE ? -2 : 1;
}

/* 前向声明：用于在下面的 find_complete_request_len 中调用 */
static int parse_content_length(const char *value, size_t *content_length);

/*
 * 检查缓冲区中是否存在一个完整的请求（包括可能的 body），
 * 如果存在返回 1，并通过 header_len/total_len 输出头部长度和总长度；
 * 如果不完整返回 0；如果 header 中 Content-Length 不合法返回 -1。
 */
static int find_complete_request_len(ConnectionBuffer *buffer, size_t *header_len, size_t *total_len) {
    char *end;
    size_t sep_len = 0;

    if (!buffer->data) return 0;
    end = strstr(buffer->data, "\r\n\r\n");
    if (end) sep_len = 4;
    else {
        end = strstr(buffer->data, "\n\n");
        if (end) sep_len = 2;
    }
    if (!end) return 0;

    size_t hlen = (size_t)(end - buffer->data) + sep_len;
    size_t content_length = 0;

    /* 在头部区间遍历每一行，查找 Content-Length（不区分大小写）并解析 */
    char *p = buffer->data;
    char *header_end_ptr = buffer->data + hlen;
    while (p < header_end_ptr) {
        char *line_end = strstr(p, "\r\n");
        size_t delim = 2;
        if (!line_end || line_end > header_end_ptr) {
            line_end = strchr(p, '\n');
            delim = 1;
        }
        if (!line_end || line_end > header_end_ptr) break;

        char *colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon) {
            size_t name_len = (size_t)(colon - p);
            if (name_len == sizeof("Content-Length") - 1 &&
                strncasecmp(p, "Content-Length", name_len) == 0) {
                /* 获取值并解析 */
                char valbuf[64];
                char *valstart = colon + 1;
                while (valstart < line_end && (*valstart == ' ' || *valstart == '\t')) valstart++;
                size_t vlen = (size_t)(line_end - valstart);
                if (vlen >= sizeof(valbuf)) vlen = sizeof(valbuf) - 1;
                memcpy(valbuf, valstart, vlen);
                valbuf[vlen] = '\0';
                size_t tmp_len = 0;
                if (!parse_content_length(valbuf, &tmp_len)) return -1;
                content_length = tmp_len;
                break;
            }
        }
        p = line_end + delim;
    }

    size_t tlen = hlen + content_length;
    if (header_len) *header_len = hlen;
    if (total_len) *total_len = tlen;
    return buffer->len >= tlen ? 1 : 0;
}

static int parse_content_length(const char *value, size_t *content_length) {
    const unsigned long long limit = (unsigned long long)((size_t)-1);
    unsigned long long result = 0;

    *content_length = 0;
    if (!value) return 1;
    while (*value == ' ' || *value == '\t') value++;
    if (!isdigit((unsigned char)*value)) return 0;

    while (isdigit((unsigned char)*value)) {
        unsigned int digit = (unsigned int)(*value - '0');
        if (result > (limit - digit) / 10) return 0;
        result = result * 10 + digit;
        value++;
    }

    while (*value == ' ' || *value == '\t') value++;
    if (*value != '\0') return 0;
    *content_length = (size_t)result;
    return 1;
}

static const char *status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

static int is_cgi_uri(const char *uri) {
    return uri && strncmp(uri, "/cgi-bin/", 9) == 0;
}

static const char *header_or_empty(HashMap *headers, const char *name) {
    const char *value = hashmap_get(headers, name);
    return value ? value : "";
}

static char *make_env_pair(const char *name, const char *value) {
    size_t len = strlen(name) + strlen(value ? value : "") + 2;
    char *pair = malloc(len);

    if (!pair) return NULL;
    snprintf(pair, len, "%s=%s", name, value ? value : "");
    return pair;
}

static void free_cgi_env(char **envp, int count) {
    if (!envp) return;
    for (int i = 0; i < count; i++) free(envp[i]);
    free(envp);
}

static int add_cgi_env(char **envp, int *idx, const char *name, const char *value) {
    envp[*idx] = make_env_pair(name, value);
    if (!envp[*idx]) return -1;
    (*idx)++;
    return 0;
}

static int parse_cgi_uri(const char *uri, char *script_name, size_t script_len,
                         char *path_info, size_t path_len, char *query, size_t query_len,
                         char *real_path, size_t real_len) {
    char uri_copy[512];
    char *query_start;
    char *path_start;
    char *extra_path;
    char *script_base;

    if (!uri || !is_cgi_uri(uri) || strstr(uri, "..")) return 0;
    if (strlen(uri) >= sizeof(uri_copy)) return 0;
    strncpy(uri_copy, uri, sizeof(uri_copy) - 1);
    uri_copy[sizeof(uri_copy) - 1] = '\0';

    query_start = strchr(uri_copy, '?');
    if (query_start) {
        *query_start++ = '\0';
        snprintf(query, query_len, "%s", query_start);
    } else {
        if (query_len > 0) query[0] = '\0';
    }

    path_start = uri_copy + 9;
    if (*path_start == '\0') return 0;
    extra_path = strchr(path_start, '/');
    if (extra_path) {
        *extra_path = '\0';
        snprintf(path_info, path_len, "/%s", extra_path + 1);
    } else {
        if (path_len > 0) path_info[0] = '\0';
    }

    script_base = path_start;
    if (*script_base == '\0') return 0;
    snprintf(script_name, script_len, "/cgi-bin/%s", script_base);
    if (snprintf(real_path, real_len, "./cgi-bin/%s", script_base) >= (int)real_len) {
        return 0;
    }
    return 1;
}

static const char *find_request_body(const char *request_raw, size_t total_len,
                                     size_t *body_len) {
    const char *body = strstr(request_raw, "\r\n\r\n");
    size_t header_len;

    if (body) {
        body += 4;
    } else {
        body = strstr(request_raw, "\n\n");
        if (!body) {
            *body_len = 0;
            return request_raw + total_len;
        }
        body += 2;
    }
    header_len = (size_t)(body - request_raw);
    if (header_len > total_len) {
        *body_len = 0;
        return request_raw + total_len;
    }
    *body_len = total_len - header_len;
    return body;
}

static int build_cgi_env(char ***env_out, const char *client_ip, const char *method,
                         const char *uri, const char *version, HashMap *headers,
                         const char *script_name, const char *path_info,
                         const char *query) {
    char **envp = calloc(22, sizeof(char *));
    char port_buf[32];
    int idx = 0;

    if (!envp) return -1;
    snprintf(port_buf, sizeof(port_buf), "%d", ECHO_PORT);
    if (add_cgi_env(envp, &idx, "CONTENT_LENGTH", header_or_empty(headers, "Content-Length")) < 0 ||
        add_cgi_env(envp, &idx, "CONTENT_TYPE", header_or_empty(headers, "Content-Type")) < 0 ||
        add_cgi_env(envp, &idx, "GATEWAY_INTERFACE", "CGI/1.1") < 0 ||
        add_cgi_env(envp, &idx, "PATH_INFO", path_info) < 0 ||
        add_cgi_env(envp, &idx, "QUERY_STRING", query) < 0 ||
        add_cgi_env(envp, &idx, "REMOTE_ADDR", client_ip) < 0 ||
        add_cgi_env(envp, &idx, "REQUEST_METHOD", method) < 0 ||
        add_cgi_env(envp, &idx, "REQUEST_URI", uri) < 0 ||
        add_cgi_env(envp, &idx, "SCRIPT_NAME", script_name) < 0 ||
        add_cgi_env(envp, &idx, "SERVER_PORT", port_buf) < 0 ||
        add_cgi_env(envp, &idx, "SERVER_PROTOCOL", version && *version ? version : "HTTP/1.1") < 0 ||
        add_cgi_env(envp, &idx, "SERVER_SOFTWARE", "Liso/1.0") < 0 ||
        add_cgi_env(envp, &idx, "HTTP_ACCEPT", header_or_empty(headers, "Accept")) < 0 ||
        add_cgi_env(envp, &idx, "HTTP_REFERER", header_or_empty(headers, "Referer")) < 0 ||
        add_cgi_env(envp, &idx, "HTTP_ACCEPT_ENCODING", header_or_empty(headers, "Accept-Encoding")) < 0 ||
        add_cgi_env(envp, &idx, "HTTP_ACCEPT_LANGUAGE", header_or_empty(headers, "Accept-Language")) < 0 ||
        add_cgi_env(envp, &idx, "HTTP_ACCEPT_CHARSET", header_or_empty(headers, "Accept-Charset")) < 0 ||
        add_cgi_env(envp, &idx, "HTTP_HOST", header_or_empty(headers, "Host")) < 0 ||
        add_cgi_env(envp, &idx, "HTTP_COOKIE", header_or_empty(headers, "Cookie")) < 0 ||
        add_cgi_env(envp, &idx, "HTTP_USER_AGENT", header_or_empty(headers, "User-Agent")) < 0 ||
        add_cgi_env(envp, &idx, "HTTP_CONNECTION", header_or_empty(headers, "Connection")) < 0) {
        free_cgi_env(envp, idx);
        return -1;
    }
    envp[idx] = NULL;
    *env_out = envp;
    return idx;
}

static int write_all_fd(int fd, const char *buf, size_t len) {
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int append_output(char **buf, size_t *len, size_t *cap, const char *chunk, size_t chunk_len) {
    char *tmp;

    if (*len + chunk_len > CGI_MAX_OUTPUT) return -1;
    if (*len + chunk_len + 1 > *cap) {
        size_t new_cap = *cap ? *cap * 2 : 4096;
        while (new_cap < *len + chunk_len + 1) new_cap *= 2;
        tmp = realloc(*buf, new_cap);
        if (!tmp) return -1;
        *buf = tmp;
        *cap = new_cap;
    }
    memcpy(*buf + *len, chunk, chunk_len);
    *len += chunk_len;
    (*buf)[*len] = '\0';
    return 0;
}

static int run_cgi_process(const char *real_path, char **envp,
                           const char *body, size_t body_len,
                           char **output, size_t *output_len) {
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    pid_t pid;
    int status;
    int env_count = 0;
    char *argv[2];
    char *out = NULL;
    size_t len = 0, cap = 0;
    time_t deadline;
    int child_done = 0;

    while (envp && envp[env_count]) env_count++;
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) goto fail;
    pid = fork();
    if (pid < 0) goto fail;
    if (pid == 0) {
        char *slash = strrchr(real_path, '/');
        argv[0] = slash ? slash + 1 : (char *)real_path;
        argv[1] = NULL;
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        execve(real_path, argv, envp);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    in_pipe[0] = -1;
    out_pipe[1] = -1;
    if (body_len > 0 && write_all_fd(in_pipe[1], body, body_len) < 0) {
        logger_error("error", "Failed to write CGI request body");
    }
    close(in_pipe[1]);
    in_pipe[1] = -1;
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);

    deadline = time(NULL) + CGI_TIMEOUT_SEC;
    while (!child_done) {
        fd_set rfds;
        struct timeval tv;
        char chunk[4096];
        int ready;

        if (time(NULL) >= deadline) {
            logger_error("error", "CGI output timeout");
            kill(pid, SIGTERM);
            sleep(1);
            if (waitpid(pid, &status, WNOHANG) == 0) kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            goto fail;
        }

        FD_ZERO(&rfds);
        FD_SET(out_pipe[0], &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        ready = select(out_pipe[0] + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            goto fail;
        }
        if (ready > 0 && FD_ISSET(out_pipe[0], &rfds)) {
            while (1) {
                ssize_t n = read(out_pipe[0], chunk, sizeof(chunk));
                if (n > 0) {
                    if (append_output(&out, &len, &cap, chunk, (size_t)n) < 0) goto fail;
                } else if (n == 0) {
                    child_done = 1;
                    break;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    goto fail;
                }
            }
        }
        if (waitpid(pid, &status, WNOHANG) == pid) {
            child_done = 1;
        }
    }
    while (1) {
        char chunk[4096];
        ssize_t n = read(out_pipe[0], chunk, sizeof(chunk));
        if (n > 0) {
            if (append_output(&out, &len, &cap, chunk, (size_t)n) < 0) goto fail;
        } else {
            break;
        }
    }
    close(out_pipe[0]);
    out_pipe[0] = -1;
    if (waitpid(pid, &status, WNOHANG) == 0) waitpid(pid, &status, 0);
    if (WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0)) {
        logger_error("error", "CGI process exited abnormally");
        goto fail;
    }
    *output = out;
    *output_len = len;
    return 0;

fail:
    if (in_pipe[0] >= 0) close(in_pipe[0]);
    if (in_pipe[1] >= 0) close(in_pipe[1]);
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (out_pipe[1] >= 0) close(out_pipe[1]);
    free(out);
    (void)env_count;
    return -1;
}

static int send_cgi_output(int sock, const char *output, size_t output_len) {
    const char *default_header = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n";
    const char *status_header = "HTTP/1.1 200 OK\r\nConnection: close\r\n";

    if (!output) output_len = 0;
    if (output_len >= 5 && strncmp(output, "HTTP/", 5) == 0) {
        return send_all(sock, output, output_len);
    }
    if ((output_len > 0 && strstr(output, "\r\n\r\n")) ||
        (output_len > 0 && strstr(output, "\n\n"))) {
        if (send_all(sock, status_header, strlen(status_header)) < 0) return -1;
        return send_all(sock, output, output_len);
    }
    if (send_all(sock, default_header, strlen(default_header)) < 0) return -1;
    if (output_len > 0 && send_all(sock, output, output_len) < 0) return -1;
    return 0;
}

static int handle_cgi_request(int sock, const char *client_ip, const char *method,
                              const char *uri, const char *version, HashMap *headers,
                              const char *request_raw, size_t total_len) {
    char script_name[256];
    char path_info[256];
    char query[1024];
    char real_path[512];
    struct stat st;
    char **envp = NULL;
    int env_count = 0;
    const char *body = NULL;
    size_t body_len = 0;
    size_t declared_body_len = 0;
    char *output = NULL;
    size_t output_len = 0;

    if (!parse_cgi_uri(uri, script_name, sizeof(script_name), path_info, sizeof(path_info),
                       query, sizeof(query), real_path, sizeof(real_path))) {
        send_all(sock, RESPONSE_404, strlen(RESPONSE_404));
        return 404;
    }
    if (stat(real_path, &st) != 0 || !S_ISREG(st.st_mode) || access(real_path, X_OK) != 0) {
        send_all(sock, RESPONSE_404, strlen(RESPONSE_404));
        return 404;
    }
    env_count = build_cgi_env(&envp, client_ip, method, uri, version, headers,
                              script_name, path_info, query);
    if (env_count < 0) {
        logger_error("error", "Failed to build CGI environment");
        send_all(sock, RESPONSE_500, strlen(RESPONSE_500));
        return 500;
    }
    body = find_request_body(request_raw, total_len, &body_len);
    if (parse_content_length(header_or_empty(headers, "Content-Length"), &declared_body_len) &&
        declared_body_len < body_len) {
        body_len = declared_body_len;
    }
    if (strcmp(method, "POST") != 0) body_len = 0;

    if (run_cgi_process(real_path, envp, body, body_len, &output, &output_len) < 0) {
        free_cgi_env(envp, env_count);
        send_all(sock, RESPONSE_500, strlen(RESPONSE_500));
        return 500;
    }
    free_cgi_env(envp, env_count);
    if (send_cgi_output(sock, output, output_len) < 0) {
        free(output);
        return 500;
    }
    free(output);
    (void)client_ip;
    return 200;
}

/*
 * 处理单个完整请求（不负责缓冲区消费）
 * 参数:
 *   sock: 客户端 socket
 *   client_ip: 客户端 IP 字符串
 *   request_raw: 完整的 HTTP 请求原始数据（包含头部和 body）
 *   total_len: request_raw 的长度
 * 返回值:
 *   1: 应保持连接（keep-alive）
 *   0: 应关闭连接
 */
static int handle_one_request(int sock, const char *client_ip,
                              const char *request_raw, size_t total_len) {
    HashMap *headers = hashmap_create();
    char method[64] = {0};
    char uri[256] = {0};
    char version[64] = {0};
    req_status status;
    int response_status = 400;
    long long content_len = -1;
    int keep_alive = 1;          // 默认保持连接（除非明确要求关闭）

    if (!headers) {
        logger_error("error", "Failed to create headers hashmap");
        return 0;
    }

    // 解析请求
    status = parse_http_request(request_raw, method, uri, version, headers);
    if (status == REQ_VALID || status == REQ_NOT_IMPL || status == REQ_VERSION_ERR) {
        const char *conn = hashmap_get(headers, "Connection");
        if (conn && strcasecmp(conn, "close") == 0) {
            keep_alive = 0;
        }
    }

    if (status == REQ_VALID) {
        if (is_cgi_uri(uri)) {
            response_status = handle_cgi_request(sock, client_ip, method, uri, version,
                                                 headers, request_raw, total_len);
            content_len = -1;
            keep_alive = 0;
        } else if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
            char file_path[512];
            if (!resolve_path(uri, file_path, sizeof(file_path))) {
                logger_error("error", "Rejected unsafe or too long URI");
                send_all(sock, RESPONSE_404, strlen(RESPONSE_404));
                response_status = 404;
                content_len = 0;
            } else {
                int ret = send_file_response(sock, file_path, method, keep_alive, &content_len);
                if (ret < 0) {
                    response_status = 500;
                    keep_alive = 0;   // 内部错误，关闭连接
                } else {
                    response_status = ret;
                }
            }
        } else { // POST
            content_len = (long long)total_len;
            int ret = send_post_raw_echo_response(sock, request_raw, total_len);
            if (ret < 0) {
                response_status = 500;
                keep_alive = 0;
            } else {
                response_status = 200;
            }
        }
    } else if (status == REQ_NOT_IMPL) {
        send_all(sock, RESPONSE_501, strlen(RESPONSE_501));
        response_status = 501;
        content_len = 0;
    } else if (status == REQ_VERSION_ERR) {
        send_all(sock, RESPONSE_505, strlen(RESPONSE_505));
        response_status = 505;
        content_len = 0;
    } else { // REQ_BAD
        send_all(sock, RESPONSE_400, strlen(RESPONSE_400));
        response_status = 400;
        content_len = 0;
    }

    // 记录访问日志（与原来完全一致）
    if (status == REQ_VALID || status == REQ_NOT_IMPL || status == REQ_VERSION_ERR) {
        logger_access(client_ip, method, uri, version, response_status, content_len);
    } else {
        logger_access(client_ip, "-", "-", "-", response_status, -1);
    }

    // 决定是否保持连接：检查 Connection 头部，且非严重错误
    if (response_status == 500 || response_status == 501) {
        keep_alive = 0;   // 500/501 错误通常关闭连接
    }

    printf("[request] fd=%d ip=%s %s %s -> %d %s, %s\n",
           sock, client_ip,
           status == REQ_BAD ? "BAD_REQUEST" : method,
           status == REQ_BAD ? "-" : uri,
           response_status, status_text(response_status),
           keep_alive ? "keep-alive" : "closing");

    hashmap_destroy(headers);
    return keep_alive;
}


/* 初始化客户端数组 */
static void init_clients() {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].sock = -1;
        buffer_init(&clients[i].buffer);
    }
    FD_ZERO(&master_set);
    max_fd = -1;
}

static int active_client_count(void) {
    int count = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].sock != -1) count++;
    }
    return count;
}

/*
 * 接受新的客户端连接
 * 参数:
 *   listen_sock: 监听socket
 */
static void accept_new_connection(int listen_sock) {
    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);
    int new_sock = accept(listen_sock, (struct sockaddr*)&cli_addr, &cli_len);
    if (new_sock < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) perror("accept");
        return;
    }

    // 检查是否超过最大连接数
    int active_count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].sock != -1) active_count++;
    }
    if (active_count >= MAX_CLIENTS) {
        // 直接拒绝，可发送 503 响应
        const char *too_busy = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
        send(new_sock, too_busy, strlen(too_busy), 0);
        close(new_sock);
        return;
    }

    // 设置非阻塞
    fcntl(new_sock, F_SETFL, O_NONBLOCK);

    // 找到空闲槽位
    int idx = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].sock == -1) { idx = i; break; }
    }
    if (idx == -1) { close(new_sock); return; } // 理论上不会发生

    clients[idx].sock = new_sock;
    buffer_init(&clients[idx].buffer);
    inet_ntop(AF_INET, &cli_addr.sin_addr, clients[idx].ip, INET_ADDRSTRLEN);
    clients[idx].keep_alive = 1;   // 默认持久连接

    FD_SET(new_sock, &master_set);
    if (new_sock > max_fd) max_fd = new_sock;

    printf("[accept] fd=%d ip=%s:%d added to select set, active_clients=%d, max_fd=%d\n",
           new_sock, clients[idx].ip, ntohs(cli_addr.sin_port),
           active_client_count(), max_fd);
}

/* 
* 处理客户端读取事件
* 参数:
*   fd: 客户端 socket描述符
*/
static void handle_client_read(int fd) {
    // 找到对应的客户端索引
    int idx = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].sock == fd) { idx = i; break; }
    }
    if (idx == -1) {
        // 异常，移除
        FD_CLR(fd, &master_set);
        close(fd);
        return;
    }

    Client *client = &clients[idx];
    char chunk[4096];
    ssize_t n;

    // 非阻塞读取
    do {
        n = recv(fd, chunk, sizeof(chunk), 0);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;   // 没有更多数据，等待下次 select
        }
        // 读错误
        goto cleanup;
    }
    if (n == 0) {
        // 客户端关闭连接
        goto cleanup;
    }

    // 将新数据追加到缓冲区
    if (buffer_reserve(&client->buffer, client->buffer.len + n + 1) < 0) {
        goto cleanup;
    }
    memcpy(client->buffer.data + client->buffer.len, chunk, n);
    client->buffer.len += n;
    client->buffer.data[client->buffer.len] = '\0';

    // 循环处理缓冲区中的所有完整请求（支持流水线）
    while (1) {
        size_t header_len = 0, total_len = 0;
        int complete = find_complete_request_len(&client->buffer, &header_len, &total_len);
        if (complete < 0) {
            // Content-Length 解析错误，返回 400 后关闭连接
            send_all(fd, RESPONSE_400, strlen(RESPONSE_400));
            logger_access(client->ip, "-", "-", "-", 400, -1);
            goto cleanup;
        }
        if (complete == 0) {
            break;   // 还没有完整请求，继续等待更多数据
        }

        // 复制完整请求（原始字节）
        char *request_raw = malloc(total_len + 1);
        if (!request_raw) goto cleanup;
        memcpy(request_raw, client->buffer.data, total_len);
        request_raw[total_len] = '\0';

        // 调用处理函数（该函数会发送响应，并返回是否应保持连接）
        int keep = handle_one_request(fd, client->ip, request_raw, total_len);
        free(request_raw);

        // 消费掉已处理的请求
        buffer_consume(&client->buffer, total_len);

        if (!keep) {
            // 根据响应决定关闭连接
            goto cleanup;
        }
    }
    return;

cleanup:
    // 关闭连接，清理资源
    close(fd);
    FD_CLR(fd, &master_set);
    buffer_free(&client->buffer);
    client->sock = -1;
    // 可选：重新计算 max_fd（可简化，跳过）
    printf("[close] fd=%d ip=%s removed from select set, active_clients=%d\n",
           fd, client->ip, active_client_count());
}


// static void process_client_connection(int sock, const char *client_ip) {
//     ConnectionBuffer buffer;
//     int keep_open = 1;

//     buffer_init(&buffer);
//     while (keep_open) {
//         size_t header_len = 0;
//         int rc = read_complete_header(sock, &buffer, &header_len);
//         if (rc == 0) break;
//         if (rc < 0) {
//             if (rc == -2) logger_error("error", "Request header too large");
//             else logger_error("error", "Failed to read request header");
//             send_all(sock, RESPONSE_400, strlen(RESPONSE_400));
//             logger_access(client_ip, "-", "-", "-", 400, -1);
//             break;
//         }

//         /* 处理缓冲区内所有已完整到达的请求（包含可能的 body） */
//         while (keep_open) {
//             size_t hlen = 0, tlen = 0;
//             int complete = find_complete_request_len(&buffer, &hlen, &tlen);
//             if (complete < 0) {
//                 /* Content-Length 解析错误 */
//                 logger_error("error", "Invalid Content-Length header");
//                 send_all(sock, RESPONSE_400, strlen(RESPONSE_400));
//                 logger_access(client_ip, "-", "-", "-", 400, -1);
//                 keep_open = 0;
//                 break;
//             }
//             if (!complete) break; /* 需要更多数据，回到 read loop */

//             /* 此时缓冲区中包含一个完整请求，交给现有处理函数处理，它会消费相应字节 */
//             keep_open = handle_one_request(sock, client_ip, buffer.data, tlen);
//             if (!keep_open) break;
//             /* 继续循环以检查是否还有下一个完整请求 */
//         }
//     }
//     buffer_free(&buffer);
// }

int main(int argc, char *argv[]) {
    int listen_sock;
    struct sockaddr_in addr;

    logger_init("access.log", "error.log");
    signal(SIGPIPE, handle_sigpipe);

    // 创建监听 socket
    listen_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (listen_sock == -1) { /* 错误处理 */ }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(ECHO_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_sock, 128);   // backlog 可稍大

    // 设置监听 socket 为非阻塞（推荐）
    fcntl(listen_sock, F_SETFL, O_NONBLOCK);

    init_clients();

    // 将监听 socket 加入 master_set
    FD_SET(listen_sock, &master_set);
    max_fd = listen_sock;

    printf("[server] Liso Web Server ready: select-based concurrency enabled, port=%d\n", ECHO_PORT);

    while (1) {
        read_set = master_set;   // select 会修改集合，必须拷贝
        if (select(max_fd + 1, &read_set, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        // 处理监听 socket 上的新连接
        if (FD_ISSET(listen_sock, &read_set)) {
            accept_new_connection(listen_sock);
        }

        // 遍历所有可能的客户端 socket（从 0 到 max_fd）
        for (int fd = 0; fd <= max_fd; fd++) {
            if (fd != listen_sock && FD_ISSET(fd, &read_set)) {
                handle_client_read(fd);
            }
        }
    }

    // 清理（实际不会走到这里）
    close(listen_sock);
    logger_close();
    return 0;
}
