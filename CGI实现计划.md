# CGI 实现计划

## 1. 目标与评分导向

本阶段目标是在当前基于 `select` 的 HTTP/1.1 服务器上实现 CGI 支持，满足课程给出的三个评分点：

1. CGI 环境变量齐全：10%
2. 完成一个带账号/密码表单的 HTML 页面，提交后由 CGI 动态生成页面并回显账号和密码：80%
3. CGI 进程出错终止时，服务器向客户端返回 500：10%

注意：课程说明中明确写到，只有完成任务点 2 的完整链路，CGI 任务才计分。因此实现顺序必须优先保证“浏览器表单 -> 服务器识别 CGI URI -> fork/exec CGI -> CGI 读取输入 -> 动态页面回显 -> 服务器返回客户端”这条链路闭环。

## 2. 参考规范

实现前阅读：

- RFC 3875: The Common Gateway Interface (CGI) Version 1.1
- RFC 2396: Uniform Resource Identifiers (URI): Generic Syntax

本项目实现以课程要求为准。RFC 用于确认 CGI 变量、URI 的 path/query 拆分、CGI 程序 stdin/stdout 交互方式，以及服务器如何包装 CGI 响应。

## 3. 项目当前基础

当前服务器文件：

- `src/echo_server.c`

当前服务器能力：

- HTTP/1.1
- GET/HEAD/POST
- 静态文件响应
- keep-alive
- select 并发
- 应用层缓冲区
- 访问日志和错误日志

新增文件建议：

- `static_site/login.html`
- `cgi-bin/login.c`
- `cgi-bin/crash.c` 或 `cgi-bin/fail.c`，用于 500 错误测试

需要修改：

- `src/echo_server.c`
- `Makefile`

## 4. CGI URI 路由规则

任何 URI 以 `/cgi-bin/` 开头，视为 CGI 请求。大小写敏感，只接受小写 `/cgi-bin/`。

示例：

- `/cgi-bin/login`
- `/cgi-bin/login?username=alice&password=123`
- `/cgi-bin/login/extra/path?x=1`

解析规则：

- `REQUEST_URI`：原始 URI，包含 path 和 query，例如 `/cgi-bin/login?x=1`
- `SCRIPT_NAME`：CGI 脚本名称，例如 `/cgi-bin/login`
- `PATH_INFO`：紧跟在脚本名后的额外路径，例如 `/extra/path`
- `QUERY_STRING`：`?` 后面的内容，不包含 `?`

初版可以只支持课程必需的 `/cgi-bin/login` 和 `/cgi-bin/login?...`。如果 URI 是 `/cgi-bin/login/extra/path`，再额外填充 `PATH_INFO`。

安全要求：

- 拒绝包含 `..` 的 CGI URI，返回 404
- 拒绝脚本路径过长，返回 404
- 不通过 shell 执行 CGI，必须使用 `execve(real_path, argv, envp)`
- 执行前使用 `stat()` 检查目标存在且是普通文件
- 使用 `access(real_path, X_OK)` 检查可执行权限

## 5. 课程要求的 21 个环境变量

课程要求的 21 项必须全部设置。注意课程材料中的 `CONTEN_ LENGTH` 应按 CGI 标准变量名实现为 `CONTENT_LENGTH`。

固定清单：

1. `CONTENT_LENGTH`
2. `CONTENT_TYPE`
3. `GATEWAY_INTERFACE`
4. `PATH_INFO`
5. `QUERY_STRING`
6. `REMOTE_ADDR`
7. `REQUEST_METHOD`
8. `REQUEST_URI`
9. `SCRIPT_NAME`
10. `SERVER_PORT`
11. `SERVER_PROTOCOL`
12. `SERVER_SOFTWARE`
13. `HTTP_ACCEPT`
14. `HTTP_REFERER`
15. `HTTP_ACCEPT_ENCODING`
16. `HTTP_ACCEPT_LANGUAGE`
17. `HTTP_ACCEPT_CHARSET`
18. `HTTP_HOST`
19. `HTTP_COOKIE`
20. `HTTP_USER_AGENT`
21. `HTTP_CONNECTION`

取值规则：

- `CONTENT_LENGTH`：请求头 `Content-Length`，没有则设为空字符串或 `"0"`，建议 POST 无 body 时设 `"0"`
- `CONTENT_TYPE`：请求头 `Content-Type`，没有则设为空字符串
- `GATEWAY_INTERFACE`：固定为 `CGI/1.1`
- `PATH_INFO`：脚本名之后的额外路径，没有则为空字符串
- `QUERY_STRING`：GET URI 中 `?` 之后的内容，没有则为空字符串
- `REMOTE_ADDR`：客户端 IP，即当前 `client_ip`
- `REQUEST_METHOD`：`GET`、`POST` 或 `HEAD`
- `REQUEST_URI`：原始 URI
- `SCRIPT_NAME`：脚本虚拟路径，例如 `/cgi-bin/login`
- `SERVER_PORT`：当前监听端口，使用 `ECHO_PORT`
- `SERVER_PROTOCOL`：固定为 `HTTP/1.1`
- `SERVER_SOFTWARE`：固定为 `Liso/1.0`
- `HTTP_*`：从请求头中直接获取，缺失则设为空字符串

请求头到环境变量映射：

- `Accept` -> `HTTP_ACCEPT`
- `Referer` -> `HTTP_REFERER`
- `Accept-Encoding` -> `HTTP_ACCEPT_ENCODING`
- `Accept-Language` -> `HTTP_ACCEPT_LANGUAGE`
- `Accept-Charset` -> `HTTP_ACCEPT_CHARSET`
- `Host` -> `HTTP_HOST`
- `Cookie` -> `HTTP_COOKIE`
- `User-Agent` -> `HTTP_USER_AGENT`
- `Connection` -> `HTTP_CONNECTION`

实现建议：

- 新增 `set_env_pair(char **envp, int *idx, const char *name, const char *value)`，内部用 `snprintf` 或动态分配生成 `NAME=value`
- 缺失值统一用空字符串，避免 CGI 程序 `getenv()` 得到 `NULL`
- `envp` 最后一项必须为 `NULL`
- `handle_cgi_request()` 返回前释放所有动态分配的环境变量字符串

## 6. HTML 表单页面

新增 `static_site/login.html`。

功能：

- 页面包含账号输入框
- 页面包含密码输入框
- 使用 POST 方法提交
- 表单目标为 `/cgi-bin/login`

示例结构：

```html
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>CGI Login</title>
</head>
<body>
  <form action="/cgi-bin/login" method="POST">
    <label>Username: <input type="text" name="username"></label>
    <label>Password: <input type="password" name="password"></label>
    <button type="submit">Submit</button>
  </form>
</body>
</html>
```

测试入口：

- 浏览器访问 `http://127.0.0.1:9999/login.html`
- 输入账号密码并提交

## 7. CGI 程序 login.c

新增 `cgi-bin/login.c`，编译后生成 `cgi-bin/login`。

功能：

- 读取环境变量 `REQUEST_METHOD`
- 读取环境变量 `CONTENT_LENGTH`
- 如果是 POST，从 `stdin` 读取 `CONTENT_LENGTH` 字节表单数据
- 如果是 GET，从 `QUERY_STRING` 读取参数
- 解析 `application/x-www-form-urlencoded`
- 支持 URL decode
- 支持 `+` 转空格
- 提取 `username` 和 `password`
- 输出动态 HTML，页面内容包含用户提交的账号和密码

CGI 输出建议：

```http
Content-Type: text/html; charset=utf-8

<!DOCTYPE html>
<html>...</html>
```

CGI 程序不需要输出完整的 `HTTP/1.1 200 OK` 状态行；服务器负责补齐状态行。

## 8. echo_server.c 修改计划

### 8.1 新增响应定义

新增：

```c
#define RESPONSE_500 "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n"
```

CGI 失败时统一返回 500，并关闭该客户端连接。

### 8.2 新增工具函数

建议新增：

- `static int is_cgi_uri(const char *uri)`
- `static int parse_cgi_uri(const char *uri, char *script_name, size_t script_len, char *path_info, size_t path_len, char *query, size_t query_len, char *real_path, size_t real_len)`
- `static char *make_env_pair(const char *name, const char *value)`
- `static int build_cgi_env(...)`
- `static void free_cgi_env(char **envp)`
- `static const char *header_or_empty(HashMap *headers, const char *name)`
- `static const char *find_request_body(const char *request_raw, size_t total_len, size_t *body_len)`
- `static int run_cgi_process(...)`
- `static int send_cgi_response(...)`

### 8.3 在 handle_one_request() 中接入 CGI

位置：解析请求并判断 `status == REQ_VALID` 后，优先判断 CGI。

逻辑：

```c
if (status == REQ_VALID && strncmp(uri, "/cgi-bin/", 9) == 0) {
    response_status = handle_cgi_request(sock, client_ip, method, uri, version,
                                         headers, request_raw, total_len);
    content_len = -1;
    keep_alive = 0;
}
```

注意：

- CGI 优先级高于静态文件处理
- 原有 POST 原样回显逻辑保留；只有 `/cgi-bin/` 请求进入 CGI
- CGI 响应建议统一 `Connection: close`，避免 CGI 输出无 `Content-Length` 时破坏 HTTP/1.1 keep-alive 边界

### 8.4 POST body 提供给 CGI stdin

从 `request_raw` 中定位请求体：

- 优先查找 `\r\n\r\n`
- 兼容查找 `\n\n`
- body 起点在 header terminator 之后
- body 长度由 `Content-Length` 决定

实现：

- 创建 `in_pipe`
- 子进程 `dup2(in_pipe[0], STDIN_FILENO)`
- 父进程关闭 `in_pipe[0]`
- 父进程向 `in_pipe[1]` 写入 body
- 写完后关闭 `in_pipe[1]`

注意：

- 如果 `CONTENT_LENGTH == 0`，仍应关闭 CGI stdin，让 CGI 程序能读到 EOF
- 写 pipe 时处理短写和 `EINTR`

### 8.5 捕获 CGI stdout

实现：

- 创建 `out_pipe`
- 子进程 `dup2(out_pipe[1], STDOUT_FILENO)`
- 父进程关闭 `out_pipe[1]`
- 父进程读取 `out_pipe[0]`
- 读取到 EOF 后结束

建议：

- `out_pipe[0]` 设置非阻塞
- 使用 `select()` 等待可读
- 动态扩展输出缓冲区
- 设置最大输出大小，例如 1MB，防止 CGI 无限输出

### 8.6 fork/execve 流程

子进程：

```c
dup2(out_pipe[1], STDOUT_FILENO);
dup2(in_pipe[0], STDIN_FILENO);
close_unused_fds();
execve(real_path, argv, envp);
_exit(127);
```

父进程：

- 关闭不用的 pipe 端
- 写入 POST body
- 读取 CGI stdout
- `waitpid()` 回收子进程
- 检查退出状态

错误判断：

- `fork()` 失败 -> 500
- `pipe()` 失败 -> 500
- `execve()` 失败 -> 子进程 `_exit(127)`，父进程看到非 0 退出 -> 500
- `WIFSIGNALED(status)` -> 500
- `WIFEXITED(status)` 且退出码非 0 -> 500
- CGI 输出超时 -> kill 子进程 -> 500

### 8.7 CGI 响应包装

如果 CGI 成功，服务器负责返回 HTTP 响应。

规则：

- 如果 CGI 输出以 `HTTP/` 开头，认为 CGI 已输出完整 HTTP 响应，服务器直接转发
- 否则服务器先发送 `HTTP/1.1 200 OK\r\nConnection: close\r\n`
- 如果 CGI 输出已经包含 CGI header，例如 `Content-Type: text/html\r\n\r\n`，服务器补状态行后发送 CGI 输出
- 如果 CGI 输出没有空行分隔 header/body，服务器发送默认头：

```http
HTTP/1.1 200 OK
Content-Type: text/html; charset=utf-8
Connection: close

```

然后发送 CGI 原始输出。

建议初版要求 `login.c` 输出 `Content-Type` 和空行，这样服务器逻辑更简单。

## 9. 500 错误处理计划

新增测试 CGI：

- `cgi-bin/fail.c`：直接 `return 1`
- 或 `cgi-bin/crash.c`：触发异常终止

服务器行为：

- 返回 `HTTP/1.1 500 Internal Server Error`
- 关闭连接
- `error.log` 记录原因，例如：
  - `CGI fork failed`
  - `CGI exec failed`
  - `CGI process exited with code 1`
  - `CGI process killed by signal 11`
  - `CGI output timeout`

## 10. Makefile 修改计划

新增目标：

```make
cgi-bin/login: cgi-bin/login.c
	$(CC) $(CFLAGS) $< -o $@

cgi-bin/fail: cgi-bin/fail.c
	$(CC) $(CFLAGS) $< -o $@
```

让 `all` 依赖 CGI 可执行文件：

```make
all : example liso_server echo_client cgi-bin/login cgi-bin/fail
```

`clean` 中删除：

```make
$(RM) cgi-bin/login cgi-bin/fail
```

## 11. 测试计划

### 11.1 编译测试

```sh
make clean && make
```

通过标准：

- `liso_server` 编译成功
- `cgi-bin/login` 编译成功
- `cgi-bin/fail` 编译成功

### 11.2 表单页面访问

启动服务器：

```sh
./liso_server
```

浏览器访问：

```text
http://127.0.0.1:9999/login.html
```

通过标准：

- 页面显示账号/密码输入框
- 表单提交到 `/cgi-bin/login`

### 11.3 POST 动态回显测试

使用浏览器提交：

- username: `alice`
- password: `123456`

通过标准：

- 响应页面包含 `alice`
- 响应页面包含 `123456`
- 服务器终端输出可显示 CGI 请求被处理
- `access.log` 记录该请求

命令行测试：

```sh
curl -i -X POST \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "username=alice&password=123456" \
  http://127.0.0.1:9999/cgi-bin/login
```

### 11.4 GET query 测试

```sh
curl -i "http://127.0.0.1:9999/cgi-bin/login?username=bob&password=abc"
```

通过标准：

- 响应页面包含 `bob`
- 响应页面包含 `abc`
- `QUERY_STRING` 被正确传递

### 11.5 环境变量完整性测试

可以临时使用 `cgi/cgi/cgi_dumper.py` 或新增 `cgi-bin/envdump.c` 输出所有环境变量。

请求：

```sh
curl -i \
  -H "Accept: text/html" \
  -H "Referer: http://example.com/from" \
  -H "Accept-Encoding: gzip" \
  -H "Accept-Language: zh-CN" \
  -H "Accept-Charset: utf-8" \
  -H "Cookie: sid=123" \
  -H "User-Agent: CGI-Test-Agent" \
  -H "Connection: close" \
  "http://127.0.0.1:9999/cgi-bin/envdump?x=1"
```

通过标准：

- 课程要求的 21 个环境变量全部出现
- 缺失请求头对应值为空字符串
- `REQUEST_URI` 保留原始 URI
- `QUERY_STRING` 为 `x=1`
- `REMOTE_ADDR` 为客户端 IP

### 11.6 CGI 异常退出测试

```sh
curl -i http://127.0.0.1:9999/cgi-bin/fail
```

通过标准：

```http
HTTP/1.1 500 Internal Server Error
```

并且 `error.log` 中记录 CGI 非 0 退出。

### 11.7 静态文件回归测试

```sh
./echo_client 127.0.0.1 9999 samples/request_get
./echo_client 127.0.0.1 9999 samples/request_head
./echo_client 127.0.0.1 9999 samples/request_post
```

通过标准：

- 原有 GET/HEAD/POST 行为不被破坏
- `/cgi-bin/` 之外的 POST 原样回显仍按原逻辑工作

### 11.8 select 并发回归测试

```sh
for i in $(seq 1 20); do
  curl -s "http://127.0.0.1:9999/cgi-bin/login?username=user$i&password=p$i" > /tmp/cgi-$i.out &
done
wait
```

通过标准：

- 所有输出文件都有对应 username/password
- 服务器没有崩溃
- 终端可看到多个 fd 被 select 管理

## 12. 实施顺序

1. 新建 `static_site/login.html`
2. 新建并单独编译 `cgi-bin/login.c`
3. 新建 `cgi-bin/fail.c`
4. 修改 `Makefile`，保证 CGI 程序随 `make` 编译
5. 在 `echo_server.c` 中加入 CGI URI 判断
6. 实现 CGI URI 拆分和路径安全检查
7. 实现 21 个环境变量构建和释放
8. 实现 `fork + execve + stdin pipe + stdout pipe`
9. 实现 CGI 输出包装和 500 错误返回
10. 接入 `handle_one_request()`，保证 CGI 优先匹配
11. 跑静态文件回归测试
12. 跑表单 POST 动态回显测试
13. 跑环境变量完整性测试
14. 跑 500 错误测试
15. 整理实验结果和分析写入课程设计报告

## 13. 报告记录建议

最终报告中建议包含：

- RFC 3875 与 RFC 2396 对实现的影响
- CGI URI 如何解析 path/query
- 21 个环境变量的来源表
- fork/execve 与 pipe 的数据流图
- 表单提交与动态回显截图
- CGI 异常退出返回 500 的截图或日志
- 原有静态文件功能未被破坏的回归测试结果
- select 并发下多个 CGI 请求能被处理的实验现象

## 14. 风险清单

- 环境变量名拼错会直接影响评分，尤其是 `CONTENT_LENGTH`、`HTTP_ACCEPT_ENCODING`
- CGI 输出没有 HTTP 状态行时，服务器必须补 `HTTP/1.1 200 OK`
- CGI 响应没有 `Content-Length` 时建议关闭连接
- 父进程必须关闭不需要的 pipe 端，否则读取 stdout 可能永远等不到 EOF
- 子进程失败后必须 `waitpid()` 回收，避免僵尸进程
- POST body 必须只写 body，不要把请求头写进 CGI stdin
- URI 中 `?` 后的 query 不属于文件路径
- URI 中的 `..` 必须拒绝
- 保留原有静态文件、POST 回显、keep-alive 和 select 并发逻辑
