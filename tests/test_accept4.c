/*
 * test_accept4.c — socket / bind / listen / accept4 / connect / shutdown
 *
 * 测试策略：完整 TCP 连接生命周期，本地环回测试
 *
 * 覆盖范围：
 *   正向：socket/bind/listen/accept4/connect，双向通信，shutdown
 *         accept4 标志（O_CLOEXEC/O_NONBLOCK），地址验证
 *   负向：无效 fd，非 socket fd，未 listen 的 socket
 *   扩展：UDP socket，socket 选项
 */

#include "test_framework.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

#define TEST_PORT  19876
#define TEST_ADDR  "127.0.0.1"
#define MSG_SRV    "hello from server"
#define MSG_CLI    "hello from client"

/* 辅助：测试 TCP 连接 */
static int test_tcp_basic(int use_cloexec, int use_nonblock)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM |
                           (use_cloexec ? SOCK_CLOEXEC : 0), 0);
    if (listen_fd < 0) return -1;

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(TEST_PORT);
    srv_addr.sin_addr.s_addr = inet_addr(TEST_ADDR);

    if (bind(listen_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 5) < 0) {
        close(listen_fd);
        return -1;
    }

    if (use_nonblock) {
        int flags = fcntl(listen_fd, F_GETFL);
        fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(listen_fd);
        usleep(100000);

        int cli_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (cli_fd < 0) _exit(1);

        struct sockaddr_in server;
        memset(&server, 0, sizeof(server));
        server.sin_family = AF_INET;
        server.sin_port = htons(TEST_PORT);
        server.sin_addr.s_addr = inet_addr(TEST_ADDR);

        if (connect(cli_fd, (struct sockaddr *)&server, sizeof(server)) < 0)
            _exit(2);

        if (send(cli_fd, MSG_CLI, strlen(MSG_CLI), 0) < 0)
            _exit(3);

        char buf[128] = {0};
        ssize_t n = recv(cli_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0 || strcmp(buf, MSG_SRV) != 0)
            _exit(4);

        close(cli_fd);
        _exit(0);
    }

    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);
    memset(&cli_addr, 0, sizeof(cli_addr));

    int conn_fd = accept4(listen_fd, (struct sockaddr *)&cli_addr,
                          &cli_len, (use_nonblock ? O_NONBLOCK : 0));

    if (conn_fd >= 0) {
        char buf[128] = {0};
        ssize_t n = recv(conn_fd, buf, sizeof(buf) - 1, 0);
        if (n > 0 && strcmp(buf, MSG_CLI) == 0) {
            send(conn_fd, MSG_SRV, strlen(MSG_SRV), 0);
        }

        int status;
        wait4(pid, &status, 0, NULL);

        close(conn_fd);
        close(listen_fd);
        return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
    }

    kill(pid, SIGKILL);
    wait4(pid, NULL, 0, NULL);
    close(listen_fd);
    return -1;
}

int main(void)
{
    TEST_START("socket: socket/bind/listen/accept4/connect/shutdown");

    /* ================================================================
     * PART 1: TCP 基本流程
     * ================================================================ */

    /* 1. socket 创建 TCP socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listen_fd >= 0, "socket(AF_INET, SOCK_STREAM) 成功");
    if (listen_fd < 0) { TEST_DONE(); }

    /* 2. bind 绑定地址 */
    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(TEST_PORT);
    srv_addr.sin_addr.s_addr = inet_addr(TEST_ADDR);
    int ret = bind(listen_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
    CHECK_RET(ret, 0, "bind 成功");

    /* 3. listen */
    ret = listen(listen_fd, 5);
    CHECK_RET(ret, 0, "listen 成功");

    /* 4. fork: 子进程做客户端，父进程做服务端 */
    pid_t pid = fork();
    if (pid == 0) {
        /* ---- 子进程: 客户端 ---- */
        close(listen_fd);
        usleep(100000);

        int cli_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (cli_fd < 0) _exit(1);

        struct sockaddr_in server;
        memset(&server, 0, sizeof(server));
        server.sin_family = AF_INET;
        server.sin_port = htons(TEST_PORT);
        server.sin_addr.s_addr = inet_addr(TEST_ADDR);

        if (connect(cli_fd, (struct sockaddr *)&server, sizeof(server)) < 0)
            _exit(2);

        if (send(cli_fd, MSG_CLI, strlen(MSG_CLI), 0) < 0)
            _exit(3);

        char buf[128] = {0};
        ssize_t n = recv(cli_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0 || strcmp(buf, MSG_SRV) != 0)
            _exit(4);

        close(cli_fd);
        _exit(0);
    }

    /* ---- 父进程: 服务端 ---- */

    /* 5. accept4 等待连接 */
    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);
    memset(&cli_addr, 0, sizeof(cli_addr));

    int conn_fd = accept4(listen_fd, (struct sockaddr *)&cli_addr, &cli_len, 0);
    CHECK(conn_fd >= 0, "accept4 成功，返回新 fd");
    if (conn_fd < 0) {
        kill(pid, SIGKILL);
        wait4(pid, NULL, 0, NULL);
        TEST_DONE();
    }

    /* 6. accept4 返回的远端地址验证 */
    CHECK(cli_addr.sin_family == AF_INET, "accept4 返回的地址族为 AF_INET");
    CHECK(cli_len > 0, "accept4 返回的地址长度 > 0");
    CHECK(ntohs(cli_addr.sin_port) != TEST_PORT,
          "accept4 返回的端口应不同于服务端端口");
    CHECK(cli_addr.sin_addr.s_addr == inet_addr(TEST_ADDR),
          "accept4 返回的地址应为 127.0.0.1");

    /* 7. 双向通信 */
    char buf[128] = {0};
    ssize_t n = recv(conn_fd, buf, sizeof(buf) - 1, 0);
    CHECK(n > 0, "服务端 recv 成功");
    CHECK(strcmp(buf, MSG_CLI) == 0, "服务端收到客户端数据正确");

    ssize_t sent = send(conn_fd, MSG_SRV, strlen(MSG_SRV), 0);
    CHECK(sent > 0, "服务端 send 成功");

    /* 8. shutdown SHUT_WR 后 recv 返回 0 */
    shutdown(conn_fd, SHUT_WR);
    char shutdown_buf[16];
    ssize_t shutdown_ret = recv(conn_fd, shutdown_buf, sizeof(shutdown_buf), 0);
    CHECK_RET(shutdown_ret, 0, "shutdown SHUT_WR 后 recv 返回 0 (EOF)");

    /* 9. 等待子进程结束 */
    int status;
    pid_t waited = wait4(pid, &status, 0, NULL);
    CHECK_RET(waited, pid, "wait4 回收客户端子进程");
    CHECK(WIFEXITED(status), "客户端子进程正常退出");
    CHECK_RET(WEXITSTATUS(status), 0, "客户端子进程退出码为 0");

    close(conn_fd);
    close(listen_fd);

    /* ================================================================
     * PART 2: accept4 标志测试
     * ================================================================ */

    /* 10. accept4 SOCK_CLOEXEC */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    bind(listen_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
    listen(listen_fd, 5);

    pid = fork();
    if (pid == 0) {
        close(listen_fd);
        usleep(50000);
        int cli_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in server;
        memset(&server, 0, sizeof(server));
        server.sin_family = AF_INET;
        server.sin_port = htons(TEST_PORT);
        server.sin_addr.s_addr = inet_addr(TEST_ADDR);
        connect(cli_fd, (struct sockaddr *)&server, sizeof(server));
        close(cli_fd);
        _exit(0);
    }

    conn_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (conn_fd >= 0) {
        int flags = fcntl(conn_fd, F_GETFD);
        CHECK(flags >= 0 && (flags & FD_CLOEXEC),
              "accept4 SOCK_CLOEXEC 设置成功");
        close(conn_fd);
    }
    wait4(pid, NULL, 0, NULL);
    close(listen_fd);

    /* ================================================================
     * PART 3: getsockname/getpeername
     * ================================================================ */

    /* 11. getsockname 获取绑定地址 */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    bind(sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr));

    struct sockaddr_in bound_addr;
    socklen_t bound_len = sizeof(bound_addr);
    ret = getsockname(sock, (struct sockaddr *)&bound_addr, &bound_len);
    CHECK_RET(ret, 0, "getsockname 获取绑定地址成功");
    CHECK(ntohs(bound_addr.sin_port) == TEST_PORT,
          "getsockname 返回的端口与绑定端口一致");
    close(sock);

    /* ================================================================
     * PART 4: UDP socket
     * ================================================================ */

    /* 12. socket UDP */
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(udp_sock >= 0, "socket(AF_INET, SOCK_DGRAM) 成功");
    if (udp_sock >= 0) {
        struct sockaddr_in udp_addr;
        memset(&udp_addr, 0, sizeof(udp_addr));
        udp_addr.sin_family = AF_INET;
        udp_addr.sin_port = htons(TEST_PORT + 1);
        udp_addr.sin_addr.s_addr = inet_addr(TEST_ADDR);

        ret = bind(udp_sock, (struct sockaddr *)&udp_addr, sizeof(udp_addr));
        CHECK_RET(ret, 0, "UDP bind 成功");

        close(udp_sock);
    }

    /* ================================================================
     * PART 5: setsockopt SO_REUSEADDR
     * ================================================================ */

    /* 13. setsockopt SO_REUSEADDR */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    CHECK_RET(ret, 0, "setsockopt SO_REUSEADDR 成功");

    if (ret == 0) {
        int val = 0;
        socklen_t len = sizeof(val);
        ret = getsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, &len);
        CHECK(ret == 0 && val == 1, "getsockopt 读回 SO_REUSEADDR 值正确");
    }
    close(sock);

    /* 14. getsockopt SO_TYPE */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock >= 0) {
        int type = 0;
        socklen_t len = sizeof(type);
        ret = getsockopt(sock, SOL_SOCKET, SO_TYPE, &type, &len);
        CHECK(ret == 0 && type == SOCK_STREAM,
              "getsockopt SO_TYPE 返回 SOCK_STREAM");
        close(sock);
    }

    /* ================================================================
     * PART 6: 负向测试
     * ================================================================ */

    /* 15. accept4 无效 fd */
    CHECK_ERR(accept4(-1, NULL, NULL, 0), EBADF,
              "accept4(-1) 应返回 EBADF");

    CHECK_ERR(accept4(9999, NULL, NULL, 0), EBADF,
              "accept4(9999) 应返回 EBADF");

    /* 16. accept4 非socket fd */
    int file_fd = openat(AT_FDCWD, "/dev/null", O_RDONLY);
    if (file_fd >= 0) {
        CHECK_ERR(accept4(file_fd, NULL, NULL, 0), ENOTSOCK,
                  "accept4 非socket fd 应返回 ENOTSOCK");
        close(file_fd);
    }

    /* 17. accept4 未 listen 的 socket */
    int unlisten_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (unlisten_fd >= 0) {
        CHECK_ERR(accept4(unlisten_fd, NULL, NULL, 0), EINVAL,
                  "accept4 未listen的socket 应返回 EINVAL");
        close(unlisten_fd);
    }

    /* 18. socket 无效协议 */
    CHECK_ERR(socket(AF_INET, SOCK_STREAM, 0xFFFF), EINVAL,
              "socket 无效协议应返回 EINVAL");

    TEST_DONE();
}
