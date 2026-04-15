#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <string.h>
#include "parse.h"

#define ECHO_PORT 15441
#define BUF_SIZE 4096

int close_socket(int sock) {
    if (close(sock)) {
        fprintf(stderr, "Failed closing socket.\n");
        return 1;
    }
    return 0;
}

void handle_signal(const int sig) {
    // 注意：这里 sock 是全局变量，但你的 main 中也有局部 sock，会有冲突。
    // 建议去掉全局变量，或者改用局部变量并忽略信号处理简化。
    // 为了简单，这里保留原样，但不影响核心功能。
    exit(0);
}

void handle_sigpipe(const int sig) {
    // 忽略 SIGPIPE
}

int main(int argc, char *argv[]) {
    int sock, client_sock;
    ssize_t readret;
    socklen_t cli_size;
    struct sockaddr_in addr, cli_addr;
    char buf[BUF_SIZE];

    fprintf(stdout, "----- Echo Server -----\n");
    
    if ((sock = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "Failed creating socket.\n");
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        fprintf(stderr, "Failed setting socket options.\n");
        return EXIT_FAILURE;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(ECHO_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *) &addr, sizeof(addr))) {
        close_socket(sock);
        fprintf(stderr, "Failed binding socket.\n");
        return EXIT_FAILURE;
    }

    if (listen(sock, 5)) {
        close_socket(sock);
        fprintf(stderr, "Error listening on socket.\n");
        return EXIT_FAILURE;
    }

    while (1) {
        cli_size = sizeof(cli_addr);
        if ((client_sock = accept(sock, (struct sockaddr *) &cli_addr, &cli_size)) == -1) {
            close(sock);
            fprintf(stderr, "Error accepting connection.\n");
            return EXIT_FAILURE;
        }

        // 读取 HTTP 请求（简单读取一次，忽略内容）
        char req_buf[BUF_SIZE];
        ssize_t n = recv(client_sock, req_buf, BUF_SIZE, 0);
        if (n < 0) {
            close_socket(client_sock);
            close_socket(sock);
            fprintf(stderr, "Error reading request.\n");
            return EXIT_FAILURE;
        }

        // 发送固定 HTTP 响应
        const char *response = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 20\r\n"
            "\r\n"
            "<h1>Hello!</h1>";
        ssize_t len = strlen(response);
        if (send(client_sock, response, len, 0) != len) {
            close_socket(client_sock);
            close_socket(sock);
            fprintf(stderr, "Error sending response.\n");
            return EXIT_FAILURE;
        }

        close_socket(client_sock);
    }

    close_socket(sock);
    return EXIT_SUCCESS;
}