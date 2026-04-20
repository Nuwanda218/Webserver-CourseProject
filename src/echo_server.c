#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define ECHO_PORT 9999
#define BUF_SIZE 4096

/* 请求解析状态 */
typedef enum {
    REQ_VALID,          /* 支持的GET/HEAD/POST方法，格式正确 */
    REQ_NOT_IMPL,       /* 方法不支持（非GET/HEAD/POST） */
    REQ_BAD             /* 格式错误 */
} req_status;

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
 * 解析HTTP请求原始报文
 * 参数:
 *   raw: 原始请求字符串（不一定以'\0'结尾）
 *   len: 报文长度
 * 返回值:
 *   REQ_VALID   : 方法为GET/HEAD/POST，且格式正确
 *   REQ_NOT_IMPL: 方法为其他（如PUT/DELETE等），且请求行格式基本正确
 *   REQ_BAD     : 格式错误（包括空请求、请求行错误、头部错误等）
 */
    req_status parse_http_request(const char *raw, size_t len) {
    /* 1. 空请求检测 */
    if (len == 0) {
        return REQ_BAD;
    }

    /* 复制一份便于安全处理，并在结尾加'\0' */
    char buf[BUF_SIZE];
    if (len >= BUF_SIZE) {
        len = BUF_SIZE - 1;
    }
    memcpy(buf, raw, len);
    buf[len] = '\0';

    char *ptr = buf;
    
    /* ---------- 解析请求行 ---------- */
    /* 查找请求行结尾 "\r\n" */
    char *request_line_end = strstr(ptr, "\r\n");
    if (!request_line_end) {
        /* 没有找到CRLF，格式错误 */
        return REQ_BAD;
    }
    *request_line_end = '\0';   /* 临时截断，便于解析 */
    
    char *method = ptr;
    /* 跳过方法名后找到空格 */
    char *sp = strchr(method, ' ');
    if (!sp) {
        return REQ_BAD;         /* 方法后无空格 */
    }
    *sp = '\0';
    char *uri = sp + 1;
    
    /* 查找URI后的空格 */
    char *sp2 = strchr(uri, ' ');
    if (!sp2) {
        return REQ_BAD;         /* URI后无空格 */
    }
    *sp2 = '\0';
    char *version = sp2 + 1;
    
    /* 校验HTTP版本格式: 必须以 "HTTP/" 开头，后面跟数字.数字 */
    if (strncmp(version, "HTTP/", 5) != 0) {
        return REQ_BAD;
    }
    char *ver_num = version + 5;
    /* 简单检查是否为 x.y 格式，允许数字和点 */
    int dot_count = 0;
    for (char *v = ver_num; *v; v++) {
        if (*v == '.') dot_count++;
        else if (*v < '0' || *v > '9') {
            return REQ_BAD;     /* 版本号包含非法字符 */
        }
    }
    if (dot_count != 1) {
        return REQ_BAD;         /* 版本号必须为两个数字 */
    }
    
    /* 检查方法名是否只包含字母（简单防御，防止特殊字符） */
    for (char *m = method; *m; m++) {
        if (!((*m >= 'A' && *m <= 'Z') || (*m >= 'a' && *m <= 'z'))) {
            return REQ_BAD;     /* 方法名包含非字母字符 */
        }
    }
    
    /* 方法名比较（大小写敏感，HTTP标准方法大写） */
    int supported = 0;
    if (strcmp(method, "GET") == 0 ||
        strcmp(method, "HEAD") == 0 ||
        strcmp(method, "POST") == 0) {
        supported = 1;
    }
    
    /* 恢复字符串，继续检查头部格式（头部格式错误也要返回400） */
    /* 将截断的'\0'恢复为原来的分隔符 */
    *request_line_end = '\r';
    /* 注意: request_line_end 指向原字符串的'\r'，后面跟'\n' */
    
    /* ---------- 检查头部格式 ---------- */
    char *header_start = request_line_end + 2;  /* 跳过"\r\n" */
    char *body_start = NULL;
    
    /* 查找空行 "\r\n\r\n" 标记头部结束 */
    char *empty_line = strstr(header_start, "\r\n\r\n");
    if (!empty_line) {
        /* 没有找到空行，格式错误 */
        return REQ_BAD;
    }
    body_start = empty_line + 4;
    
    /* 检查每个头部字段行: 格式必须是 "Name: value\r\n" */
    char *line = header_start;
    while (line < empty_line) {
        char *line_end = strstr(line, "\r\n");
        if (!line_end) {
            return REQ_BAD;     /* 头部行没有以CRLF结尾 */
        }
        *line_end = '\0';
        
        /* 查找冒号 */
        char *colon = strchr(line, ':');
        if (!colon) {
            return REQ_BAD;     /* 头部字段缺少冒号 */
        }
        if (colon == line) {
            return REQ_BAD;     /* 冒号前没有字段名 */
        }
        /* 可选：检查字段名是否包含空格？根据RFC可以包含连字符等，这里不做过多限制 */
        
        /* 恢复换行符 */
        *line_end = '\r';
        line = line_end + 2;    /* 下一行 */
    }
    
    /* 额外检测：URI不能为空（即方法后直接CRLF） */
    if (strlen(uri) == 0) {
        return REQ_BAD;
    }
    
    /* 根据方法支持情况返回结果 */
    if (supported) {
        return REQ_VALID;
    } else {
        return REQ_NOT_IMPL;
    }
}

/* 
 * 构造并发送HTTP响应
 * 参数:
 *   sock: 客户端socket
 *   status: 解析状态
 *   request_raw: 原始请求字符串（仅在REQ_VALID时使用，用于echo）
 *   raw_len: 原始请求长度
 */
void send_http_response(int sock, req_status status, const char *request_raw, size_t raw_len) {
    char response_buf[BUF_SIZE * 2];   /* 响应缓冲区足够大 */
    int response_len = 0;
    
    if (status == REQ_VALID) {
        /* Echo: 返回200 OK，body为完整原始请求报文 */
        const char *status_line = "HTTP/1.1 200 OK\r\n";
        const char *content_type = "Content-Type: message/http\r\n";
        char content_length_header[64];
        snprintf(content_length_header, sizeof(content_length_header),
                 "Content-Length: %zu\r\n", raw_len);
        const char *empty_line = "\r\n";
        
        /* 拼接响应头部 */
        snprintf(response_buf, sizeof(response_buf), "%s%s%s%s",
                 status_line, content_type, content_length_header, empty_line);
        response_len = strlen(response_buf);
        
        /* 检查剩余空间是否足够容纳body */
        if (response_len + raw_len < sizeof(response_buf)) {
            memcpy(response_buf + response_len, request_raw, raw_len);
            response_len += raw_len;
        } else {
            /* 理论上不会发生，因为BUF_SIZE足够大，但如果发生则降级处理 */
            fprintf(stderr, "Response buffer too small for echo body\n");
            const char *fallback = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
            send(sock, fallback, strlen(fallback), 0);
            return;
        }
    } 
    else if (status == REQ_NOT_IMPL) {
        /* 501 Not Implemented */
        const char *response = "HTTP/1.1 501 Not Implemented\r\n\r\n";
        response_len = strlen(response);
        snprintf(response_buf, sizeof(response_buf), "%s", response);
    } 
    else { /* REQ_BAD */
        /* 400 Bad Request */
        const char *response = "HTTP/1.1 400 Bad Request\r\n\r\n";
        response_len = strlen(response);
        snprintf(response_buf, sizeof(response_buf), "%s", response);
    }
    
    ssize_t sent = send(sock, response_buf, response_len, 0);
    if (sent != response_len) {
        fprintf(stderr, "Warning: incomplete send (%zd/%d)\n", sent, response_len);
    }
}

int main(int argc, char *argv[]) {
    int sock, client_sock;
    struct sockaddr_in addr, cli_addr;
    socklen_t cli_size;
    char buf[BUF_SIZE];
    
    /* 忽略SIGPIPE信号 */
    signal(SIGPIPE, handle_sigpipe);
    
    fprintf(stdout, "----- Echo Web Server (HTTP/1.1) -----\n");
    fprintf(stdout, "Listening on port %d\n", ECHO_PORT);
    
    /* 创建socket */
    if ((sock = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "Failed creating socket.\n");
        return EXIT_FAILURE;
    }
    
    /* 设置端口复用 */
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        fprintf(stderr, "Failed setting socket options.\n");
        close_socket(sock);
        return EXIT_FAILURE;
    }
    
    /* 绑定地址和端口 */
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ECHO_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr))) {
        close_socket(sock);
        fprintf(stderr, "Failed binding socket.\n");
        return EXIT_FAILURE;
    }
    
    /* 监听 */
    if (listen(sock, 5)) {
        close_socket(sock);
        fprintf(stderr, "Error listening on socket.\n");
        return EXIT_FAILURE;
    }
    
    /* 主循环：接受连接并处理 */
    while (1) {
        cli_size = sizeof(cli_addr);
        if ((client_sock = accept(sock, (struct sockaddr *)&cli_addr, &cli_size)) == -1) {
            /* 发生错误时继续，不退出 */
            perror("accept");
            continue;
        }
        
        /* 获取客户端IP地址 */
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli_addr.sin_addr, client_ip, sizeof(client_ip));
        fprintf(stdout, "Connection from %s:%d\n", client_ip, ntohs(cli_addr.sin_port));
        
        /* 读取HTTP请求 */
        ssize_t n = recv(client_sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (n < 0) perror("recv");
            close_socket(client_sock);
            continue;
        }
        buf[n] = '\0';  /* 添加字符串结束符，便于打印和解析 */
        
        /* 打印请求（调试用） */
        fprintf(stdout, "--- Request (%zd bytes) ---\n%s\n--- End Request ---\n", n, buf);
        
        /* 解析请求 */
        req_status status = parse_http_request(buf, n);
        
        /* 根据解析结果发送响应 */
        if (status == REQ_VALID) {
            fprintf(stdout, "-> Responding with 200 OK (echo)\n");
            send_http_response(client_sock, REQ_VALID, buf, n);
        } else if (status == REQ_NOT_IMPL) {
            fprintf(stdout, "-> Responding with 501 Not Implemented\n");
            send_http_response(client_sock, REQ_NOT_IMPL, NULL, 0);
        } else {
            fprintf(stdout, "-> Responding with 400 Bad Request\n");
            send_http_response(client_sock, REQ_BAD, NULL, 0);
        }
        
        close_socket(client_sock);
        fprintf(stdout, "Connection closed.\n\n");
    }
    
    close_socket(sock);
    return EXIT_SUCCESS;
}