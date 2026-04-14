/*
 * test_pwritev2.c — pwritev / pwritev2 写入功能验证
 *
 * 测试目的：验证 pwritev 和 pwritev2 是否真正将 iovec 数据写入文件。
 * 已知 bug：sys_pwritev2 的实现从 sys_preadv2 复制过来后，内部调用
 * 仍然是 read_at 而非 write_at，导致写入操作静默丢弃数据。
 *
 * 覆盖范围：
 *   1. pwrite 基准（非向量化写入）
 *   2. pwritev 向量化写入 + 回读验证
 *   3. pwritev2 直接 syscall 写入 + 回读验证
 */

#include "test_framework.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/syscall.h>

#define TMPFILE_PW   "/tmp/starry_test_pwrite"
#define TMPFILE_PWV  "/tmp/starry_test_pwritev"
#define TMPFILE_PWV2 "/tmp/starry_test_pwritev2"

/* musl 没有 pwritev2 的 wrapper，直接用 syscall。
 * 内核定义为 SYSCALL_DEFINE6(pwritev2, fd, vec, vlen, pos_l, pos_h, flags)，
 * offset 拆成 pos_l + pos_h 两个参数，64 位上 pos_h = 0。 */
static ssize_t my_pwritev2(int fd, const struct iovec *iov, int iovcnt,
                           off_t offset, int flags)
{
    return syscall(SYS_pwritev2, fd, iov, iovcnt,
                   (unsigned long)offset, (unsigned long)0, flags);
}

int main(void)
{
    TEST_START("pwritev2: pwritev/pwritev2 写入功能验证");

    /* ================================================================
     * PART 1: pwrite 基准测试
     * ================================================================ */

    int fd = open(TMPFILE_PW, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "pwrite 基准: 创建文件");

    const char *data = "hello";
    ssize_t n = pwrite(fd, data, 5, 0);
    CHECK_RET(n, 5, "pwrite 写入 5 字节");

    char buf[8] = {0};
    ssize_t r = pread(fd, buf, 5, 0);
    CHECK_RET(r, 5, "pread 读回 5 字节");
    CHECK(memcmp(buf, "hello", 5) == 0, "pwrite 数据一致");

    close(fd);
    unlink(TMPFILE_PW);

    /* ================================================================
     * PART 2: pwritev 向量化写入
     *
     * 操作：先 pwrite 写入 "00000000"，再用 pwritev 覆盖为 "AAAABBBB"
     * 观测：pread 回读内容
     * 验证：内容为 "AAAABBBB"（非原始的 "00000000"）
     * ================================================================ */

    fd = open(TMPFILE_PWV, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "pwritev 测试: 创建文件");

    const char *zeros = "00000000";
    pwrite(fd, zeros, 8, 0);

    char data1[] = "AAAA";
    char data2[] = "BBBB";
    struct iovec iov[2];
    iov[0].iov_base = data1;
    iov[0].iov_len = 4;
    iov[1].iov_base = data2;
    iov[1].iov_len = 4;

    n = pwritev(fd, iov, 2, 0);
    CHECK_RET(n, 8, "pwritev 写入 8 字节 (2 个 iovec)");

    char buf2[16] = {0};
    r = pread(fd, buf2, 8, 0);
    CHECK_RET(r, 8, "pread 读回 8 字节");
    CHECK(memcmp(buf2, "AAAABBBB", 8) == 0,
          "pwritev 数据正确 (AAAABBBB 非 00000000)");

    close(fd);
    unlink(TMPFILE_PWV);

    /* ================================================================
     * PART 3: pwritev2 直接 syscall
     *
     * 操作：先写入 "00000000"，再用 pwritev2 覆盖为 "TESTDATA"
     * 观测：pread 回读内容
     * 验证：内容为 "TESTDATA"
     * ================================================================ */

    fd = open(TMPFILE_PWV2, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "pwritev2 测试: 创建文件");

    pwrite(fd, zeros, 8, 0);

    char data3[] = "TESTDATA";
    struct iovec iov2;
    iov2.iov_base = data3;
    iov2.iov_len = 8;

    n = my_pwritev2(fd, &iov2, 1, 0, 0);
    CHECK_RET(n, 8, "pwritev2 写入 8 字节");

    char buf3[16] = {0};
    r = pread(fd, buf3, 8, 0);
    CHECK_RET(r, 8, "pread 读回 8 字节");
    CHECK(memcmp(buf3, "TESTDATA", 8) == 0,
          "pwritev2 数据正确 (TESTDATA 非 00000000)");

    close(fd);
    unlink(TMPFILE_PWV2);

    TEST_DONE();
}
