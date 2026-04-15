/*
 * test_accept_addr.c - accept4 应返回对端地址而非本地地址
 *
 * 对应 bug: sys_accept4 使用 local_addr() 而非 peer_addr()
 * 对应 PR:  https://github.com/rcore-os/tgoskits/pull/203
 *
 * 编译: riscv64-linux-musl-gcc -static -o test_accept_addr test_accept_addr.c
 */

#include "test_framework.h"
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>

#define TEST_PORT 19876

int main(void) {
    TEST_START("accept: returns peer address");

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(srv >= 0, "server socket");

    struct sockaddr_in saddr = {
        .sin_family = AF_INET,
        .sin_port = htons(TEST_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    CHECK(bind(srv, (struct sockaddr *)&saddr, sizeof(saddr)) == 0, "bind");
    CHECK(listen(srv, 1) == 0, "listen");

    pid_t pid = fork();
    CHECK(pid >= 0, "fork");

    if (pid == 0) {
        close(srv);
        int c = socket(AF_INET, SOCK_STREAM, 0);
        connect(c, (struct sockaddr *)&saddr, sizeof(saddr));
        usleep(200000);
        close(c);
        _exit(0);
    }

    struct sockaddr_in accepted = {0};
    socklen_t alen = sizeof(accepted);
    int cli = accept(srv, (struct sockaddr *)&accepted, &alen);
    CHECK(cli >= 0, "accept succeeds");

    /* accept 返回的地址应该是对端（客户端）地址 */
    struct sockaddr_in peer = {0};
    socklen_t plen = sizeof(peer);
    getpeername(cli, (struct sockaddr *)&peer, &plen);

    /* 核心检查：accept 地址 == getpeername 地址 */
    CHECK(accepted.sin_addr.s_addr == peer.sin_addr.s_addr,
          "accept addr.ip == getpeername addr.ip");
    CHECK(accepted.sin_port == peer.sin_port,
          "accept addr.port == getpeername addr.port");

    /* accept 返回的端口不应该是服务端口（那是 local_addr 的 bug 表现） */
    CHECK(accepted.sin_port != htons(TEST_PORT),
          "accept port is client ephemeral port (not server port)");

    close(cli);
    close(srv);
    int st;
    waitpid(pid, &st, 0);

    TEST_DONE();
}
