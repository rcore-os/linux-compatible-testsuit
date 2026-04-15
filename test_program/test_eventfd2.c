/*
 * test_eventfd2.c — eventfd2 (via eventfd)
 *
 * 正向: 创建eventfd → write累加计数器 → read读取并清零 → 验证计数器语义
 *       EFD_CLOEXEC / EFD_NONBLOCK / EFD_SEMAPHORE 标志验证
 * 负向: 无效flags / write最大值溢出 / 非阻塞读空计数器 / 缓冲区过小
 */

#include "test_framework.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

int main(void)
{
    TEST_START("eventfd: eventfd2 read/write/flags");

    /* ============== 正向测试 ============== */

    /* 1. 基本创建: initval=0, flags=0 */
    int efd = eventfd(0, 0);
    CHECK(efd >= 0, "eventfd(0, 0) 创建成功");
    if (efd < 0) {
        printf("  FATAL: eventfd 创建失败，终止\n");
        TEST_DONE();
    }

    /* 2. write 累加计数器: 写入 42 */
    uint64_t val = 42;
    ssize_t wret = write(efd, &val, sizeof(val));
    CHECK_RET(wret, (ssize_t)sizeof(val), "write 42 成功");

    /* 3. 再次 write 累加: 写入 8，计数器应为 50 */
    val = 8;
    wret = write(efd, &val, sizeof(val));
    CHECK_RET(wret, (ssize_t)sizeof(val), "write 8 成功");

    /* 4. read 读取计数器: 非semaphore模式下应返回总和并清零 */
    uint64_t read_val = 0;
    ssize_t rret = read(efd, &read_val, sizeof(read_val));
    CHECK_RET(rret, (ssize_t)sizeof(read_val), "read 返回8字节");
    CHECK_RET(read_val, 50ULL, "read 值为 42+8=50 (计数器累加正确)");

    /* 5. 清零后再读应阻塞(因为已经用 EFD_NONBLOCK 测试，见下) */
    close(efd);

    /* 6. initval 非零: 创建后立即读取应得到 initval */
    efd = eventfd(100, 0);
    CHECK(efd >= 0, "eventfd(100, 0) 创建成功");
    read_val = 0;
    rret = read(efd, &read_val, sizeof(read_val));
    CHECK_RET(rret, (ssize_t)sizeof(read_val), "read initval 返回8字节");
    CHECK_RET(read_val, 100ULL, "read 值为 initval=100");
    close(efd);

    /* 7. EFD_NONBLOCK: 计数器为0时 read 应返回 EAGAIN */
    efd = eventfd(0, EFD_NONBLOCK);
    CHECK(efd >= 0, "eventfd EFD_NONBLOCK 创建成功");
    read_val = 0;
    errno = 0;
    CHECK_ERR(read(efd, &read_val, sizeof(read_val)), EAGAIN,
              "非阻塞 read 空计数器应返回 EAGAIN");
    close(efd);

    /* 8. EFD_CLOEXEC: 验证 FD_CLOEXEC 标志已设置 */
    efd = eventfd(0, EFD_CLOEXEC);
    CHECK(efd >= 0, "eventfd EFD_CLOEXEC 创建成功");
    int fd_flags = fcntl(efd, F_GETFD);
    CHECK(fd_flags >= 0 && (fd_flags & FD_CLOEXEC),
          "EFD_CLOEXEC: FD_CLOEXEC 标志已设置");
    close(efd);

    /* 9. EFD_SEMAPHORE 模式: read 返回 1 并递减计数器而非清零 */
    efd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK);
    CHECK(efd >= 0, "eventfd EFD_SEMAPHORE|EFD_NONBLOCK 创建成功");

    /* 写入 3，计数器变为 3 */
    val = 3;
    wret = write(efd, &val, sizeof(val));
    CHECK_RET(wret, (ssize_t)sizeof(val), "semaphore: write 3 成功");

    /* 第一次 read: 应返回 1，计数器变 2 */
    read_val = 0;
    rret = read(efd, &read_val, sizeof(read_val));
    CHECK_RET(rret, (ssize_t)sizeof(read_val), "semaphore: read 返回8字节");
    CHECK_RET(read_val, 1ULL, "semaphore: 第一次 read 返回 1");

    /* 第二次 read: 应返回 1，计数器变 1 */
    read_val = 0;
    rret = read(efd, &read_val, sizeof(read_val));
    CHECK_RET(read_val, 1ULL, "semaphore: 第二次 read 返回 1");

    /* 第三次 read: 应返回 1，计数器变 0 */
    read_val = 0;
    rret = read(efd, &read_val, sizeof(read_val));
    CHECK_RET(read_val, 1ULL, "semaphore: 第三次 read 返回 1");

    /* 第四次 read: 计数器已为0，非阻塞应 EAGAIN */
    read_val = 0;
    errno = 0;
    CHECK_ERR(read(efd, &read_val, sizeof(read_val)), EAGAIN,
              "semaphore: 计数器耗尽后 read 应返回 EAGAIN");
    close(efd);

    /* 10. write 后非阻塞 read 多次: 非 semaphore 模式下 read 一次清零 */
    efd = eventfd(0, EFD_NONBLOCK);
    val = 5;
    wret = write(efd, &val, sizeof(val));
    (void)wret;
    read_val = 0;
    rret = read(efd, &read_val, sizeof(read_val));
    (void)rret;
    CHECK_RET(read_val, 5ULL, "非semaphore: read 返回完整计数器值");

    /* 第二次 read 应 EAGAIN (已被清零) */
    errno = 0;
    CHECK_ERR(read(efd, &read_val, sizeof(read_val)), EAGAIN,
              "非semaphore: 清零后 read 应返回 EAGAIN");
    close(efd);

    /* ============== 负向测试 ============== */

    /* 11. read 缓冲区小于8字节应返回 EINVAL */
    efd = eventfd(0, EFD_NONBLOCK);
    val = 1;
    wret = write(efd, &val, sizeof(val));
    (void)wret;
    errno = 0;
    {
        uint32_t small_buf;
        CHECK_ERR(read(efd, &small_buf, sizeof(small_buf)), EINVAL,
                  "read 缓冲区 < 8 字节应返回 EINVAL");
    }
    close(efd);

    /* 12. write 缓冲区小于8字节应返回 EINVAL */
    efd = eventfd(0, 0);
    errno = 0;
    {
        uint32_t small_val = 1;
        CHECK_ERR(write(efd, &small_val, sizeof(small_val)), EINVAL,
                  "write 缓冲区 < 8 字节应返回 EINVAL");
    }
    close(efd);

    /* 13. write 0xFFFFFFFFFFFFFFFF 应返回 EINVAL */
    efd = eventfd(0, 0);
    errno = 0;
    val = 0xFFFFFFFFFFFFFFFFULL;
    CHECK_ERR(write(efd, &val, sizeof(val)), EINVAL,
              "write 0xFFFFFFFFFFFFFFFF 应返回 EINVAL");
    close(efd);

    /* 14. 无效 flags (使用未定义的位) 应返回 EINVAL */
    errno = 0;
    CHECK_ERR(eventfd(0, 0xDEAD), EINVAL,
              "eventfd 无效 flags 应返回 EINVAL");

    /* 15. write 导致计数器溢出 (非阻塞模式下) 应返回 EAGAIN */
    efd = eventfd(0, EFD_NONBLOCK);
    /* 计数器最大值为 0xFFFFFFFFFFFFFFFE，写入一个接近最大值的数 */
    val = 0xFFFFFFFFFFFFFFFEULL;
    wret = write(efd, &val, sizeof(val));
    /* 如果写入成功，再写 1 应该溢出 */
    if (wret == (ssize_t)sizeof(val)) {
        val = 1;
        errno = 0;
        CHECK_ERR(write(efd, &val, sizeof(val)), EAGAIN,
                  "write 导致计数器溢出应返回 EAGAIN");
    }
    close(efd);

    /* 16. 对已关闭的 fd 操作应返回 EBADF */
    efd = eventfd(0, EFD_NONBLOCK);
    close(efd);
    errno = 0;
    val = 1;
    CHECK_ERR(write(efd, &val, sizeof(val)), EBADF,
              "write 已关闭的 fd 应返回 EBADF");

    TEST_DONE();
}
