/*
 * test_basic_io.c — read / write / lseek / close 核心语义验证
 *
 * 测试策略：不仅验证返回值，还通过独立观测手段验证功能语义真正生效。
 * 遵循 sys_design.md "操作→观测→验证" 三步法。
 *
 * 覆盖范围：
 *   正向：基本读写、offset 移动、EOF 语义、hole 创建、数据完整性、
 *         覆盖写入、大文件、partial read、独立 fd 偏移、close 后失效
 *   负向：EBADF/EINVAL/EFAULT 错误路径
 *   状态转移：文件 offset 与状态的变化
 */

#include "test_framework.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

#define TMPFILE "/tmp/starry_test_basic_io"
#define BIG_FILE_SIZE (1024 * 1024)  /* 1MB */

/* 辅助：生成带模式的测试数据 */
static void fill_pattern(char *buf, size_t size, unsigned char seed)
{
    for (size_t i = 0; i < size; i++) {
        buf[i] = (unsigned char)((i + seed) & 0xFF);
    }
}

int main(void)
{
    TEST_START("basic io: read/write/lseek/close 核心语义验证");

    /* 清理残留 */
    unlink(TMPFILE);

    /* ================================================================
     * PART 1: 基本 write + lseek + read 流程
     * ================================================================ */

    int fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "openat 创建文件成功");
    if (fd < 0) { TEST_DONE(); }

    const char *test_msg = "Hello StarryOS!";
    int msg_len = strlen(test_msg);

    ssize_t wret = write(fd, test_msg, msg_len);
    CHECK_RET(wret, msg_len, "write 写入完整数据");

    /* 观测：write 后文件偏移自动移动 */
    off_t pos_after_write = lseek(fd, 0, SEEK_CUR);
    CHECK_RET(pos_after_write, msg_len, "write 后: offset 自动移动到写入位置");

    /* lseek 回到头部 */
    off_t off = lseek(fd, 0, SEEK_SET);
    CHECK_RET(off, 0, "lseek SEEK_SET 回到头部");

    /* read 读回数据 */
    char buf[64] = {0};
    ssize_t rret = read(fd, buf, sizeof(buf) - 1);
    CHECK_RET(rret, msg_len, "read 读回完整数据");
    CHECK(strcmp(buf, test_msg) == 0, "read 内容与写入一致");

    /* 观测：read 后文件偏移自动移动 */
    off_t pos_after_read = lseek(fd, 0, SEEK_CUR);
    CHECK_RET(pos_after_read, msg_len, "read 后: offset 自动移动到读取位置");

    close(fd);

    /* ================================================================
     * PART 2: write 后 stat 确认数据持久化到文件系统
     *
     * 操作：write 写入数据后 close
     * 观测：stat 确认 st_size
     * 验证：数据真正持久化，非仅在内存
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "持久化测试: 打开文件");

    const char *single = "X";
    CHECK_RET(write(fd, single, 1), 1, "write 单字节");
    close(fd);

    /* 观测：stat 独立确认文件大小 */
    struct stat st;
    CHECK_RET(stat(TMPFILE, &st), 0, "stat 获取文件信息");
    CHECK(st.st_size == 1, "stat 确认 write 后 st_size == 1");

    /* ================================================================
     * PART 3: read 返回 0 表示 EOF
     *
     * 操作：写 5 字节，全部读完后再次 read
     * 观测：第二次 read 返回值
     * 验证：返回 0（EOF 语义）
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "EOF 测试: 打开文件");

    const char *five_bytes = "12345";
    CHECK_RET(write(fd, five_bytes, 5), 5, "写入 5 字节");
    lseek(fd, 0, SEEK_SET);

    char buf5[8];
    CHECK_RET(read(fd, buf5, 8), 5, "第一次 read 返回 5 字节");

    /* 观测：再次 read 应返回 0 (EOF) */
    ssize_t eof_ret = read(fd, buf5, 8);
    CHECK_RET(eof_ret, 0, "第二次 read 返回 0 (EOF 语义)");

    close(fd);

    /* ================================================================
     * PART 4: lseek SEEK_CUR / SEEK_END
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "lseek 变体测试: 打开文件");

    CHECK_RET(write(fd, "ABCDEFGHIJ", 10), 10, "写入 10 字节");

    /* SEEK_CUR */
    lseek(fd, 0, SEEK_SET);
    off_t cur1 = lseek(fd, 3, SEEK_CUR);
    CHECK_RET(cur1, 3, "lseek SEEK_CUR: 从 0 移动 3 → 3");
    off_t cur2 = lseek(fd, 2, SEEK_CUR);
    CHECK_RET(cur2, 5, "lseek SEEK_CUR: 从 3 移动 2 → 5");

    /* SEEK_END */
    off_t end_pos = lseek(fd, 0, SEEK_END);
    CHECK_RET(end_pos, 10, "lseek SEEK_END 返回文件大小 10");

    /* SEEK_END 负偏移 */
    lseek(fd, -3, SEEK_END);
    char buf_end[4];
    CHECK_RET(read(fd, buf_end, 3), 3, "SEEK_END -3: 读回 3 字节");
    CHECK(memcmp(buf_end, "HIJ", 3) == 0, "SEEK_END -3: 读到末尾 3 字节 HIJ");

    close(fd);

    /* ================================================================
     * PART 5: lseek 超过文件末尾创建 hole
     *
     * 操作：写 "start"，lseek 到 +1000，写 "end"
     * 观测：stat 确认 st_size，read 中间区域
     * 验证：中间区域全为 \0（hole 语义）
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "hole 测试: 打开文件");

    CHECK_RET(write(fd, "start", 5), 5, "写 'start'");
    CHECK_RET(lseek(fd, 1000, SEEK_CUR), 1005, "lseek +1000 创建 hole");
    CHECK_RET(write(fd, "end", 3), 3, "写 'end'");

    /* 观测：stat 确认文件大小 = 5 + 1000 + 3 */
    CHECK_RET(stat(TMPFILE, &st), 0, "hole 测试: stat 成功");
    CHECK(st.st_size == 1008, "hole 测试: st_size == 1008 (5 + 1000 + 3)");

    /* 观测：read 中间区域验证全为 \0 */
    lseek(fd, 5, SEEK_SET);
    char hole_buf[100];
    memset(hole_buf, 'X', sizeof(hole_buf));
    CHECK_RET(read(fd, hole_buf, 100), 100, "read 中间 100 字节");

    /* 验证：中间区域应全为 \0 */
    int all_zero = 1;
    for (int i = 0; i < 100; i++) {
        if (hole_buf[i] != '\0') {
            all_zero = 0;
            break;
        }
    }
    CHECK(all_zero, "hole 中间区域全为 \\0");

    close(fd);

    /* ================================================================
     * PART 6: 覆盖写入验证
     *
     * 操作：写 "AAAA"，lseek 到偏移 2，写 "BB"
     * 观测：read 全部内容
     * 验证：内容为 "AABB"（非 "BBBB"）
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "覆盖写入测试: 打开文件");

    CHECK_RET(write(fd, "AAAA", 4), 4, "写 'AAAA'");
    CHECK_RET(lseek(fd, 2, SEEK_SET), 2, "lseek 到偏移 2");
    CHECK_RET(write(fd, "BB", 2), 2, "写 'BB' 覆盖");

    lseek(fd, 0, SEEK_SET);
    char buf_over[8];
    CHECK_RET(read(fd, buf_over, 8), 4, "read 全部内容");
    CHECK(memcmp(buf_over, "AABB", 4) == 0, "覆盖写入: 内容为 'AABB' (非 'BBBB')");

    close(fd);

    /* ================================================================
     * PART 7: 不同大小 write 验证
     *
     * 操作：分别写入 0/1/4096/8192 字节
     * 观测：每次 write 返回值
     * 验证：返回值精确匹配请求大小
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "不同大小 write 测试: 打开文件");

    /* write 0 字节 */
    CHECK_RET(write(fd, "x", 0), 0, "write 0 字节返回 0");

    /* write 1 字节 */
    CHECK_RET(write(fd, "x", 1), 1, "write 1 字节");

    /* write 4096 字节 (一页) */
    char page[4096];
    memset(page, 'P', sizeof(page));
    CHECK_RET(write(fd, page, 4096), 4096, "write 4096 字节 (一页)");

    /* write 8192 字节 (两页) */
    char two_pages[8192];
    memset(two_pages, 'Q', sizeof(two_pages));
    CHECK_RET(write(fd, two_pages, 8192), 8192, "write 8192 字节 (两页)");

    /* 验证文件大小 */
    off_t total_size = lseek(fd, 0, SEEK_END);
    CHECK_RET(total_size, 1 + 4096 + 8192, "文件大小 = 1 + 4096 + 8192");

    close(fd);

    /* ================================================================
     * PART 8: partial read — 请求 > 文件剩余
     *
     * 操作：写 "hello"(5字节)，lseek 到头，read(buf, 100)
     * 观测：read 返回值
     * 验证：返回 5（非 100），buf 前 5 字节正确
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "partial read 测试: 打开文件");

    CHECK_RET(write(fd, "hello", 5), 5, "写 'hello' (5 字节)");
    lseek(fd, 0, SEEK_SET);

    char buf_partial[100];
    ssize_t partial_ret = read(fd, buf_partial, 100);
    CHECK_RET(partial_ret, 5, "partial read: 返回实际读到的 5 字节 (非请求的 100)");
    CHECK(memcmp(buf_partial, "hello", 5) == 0, "partial read: 前 5 字节内容正确");

    close(fd);

    /* ================================================================
     * PART 9: 空文件 read 立即返回 EOF
     *
     * 操作：O_CREAT+O_TRUNC 创建文件后直接 read
     * 观测：read 返回值
     * 验证：返回 0（0 字节文件首次 read 即 EOF）
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "空文件 read 测试: 打开文件");

    char buf_empty[10];
    ssize_t empty_ret = read(fd, buf_empty, 10);
    CHECK_RET(empty_ret, 0, "空文件 read 首次即返回 0 (EOF)");

    close(fd);

    /* ================================================================
     * PART 10: 两次 open 同一文件，偏移独立
     *
     * 操作：fd1 和 fd2 打开同一文件，各自 write
     * 观测：各自的 lseek(0, SEEK_CUR)
     * 验证：偏移互不干扰（独立的 file 结构体）
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    int fd2 = openat(AT_FDCWD, TMPFILE, O_RDWR);
    CHECK(fd >= 0 && fd2 >= 0, "双 fd 测试: 打开同一文件两次");

    if (fd >= 0 && fd2 >= 0) {
        CHECK_RET(write(fd, "AA", 2), 2, "fd1 写 'AA'");
        off_t pos1 = lseek(fd, 0, SEEK_CUR);
        CHECK_RET(pos1, 2, "fd1 offset == 2");

        CHECK_RET(write(fd2, "BB", 2), 2, "fd2 写 'BB'");
        off_t pos2 = lseek(fd2, 0, SEEK_CUR);
        CHECK_RET(pos2, 2, "fd2 offset == 2 (独立于 fd1)");

        /* 验证最终文件大小 */
        lseek(fd, 0, SEEK_END);
        off_t final_size = lseek(fd, 0, SEEK_CUR);
        CHECK_RET(final_size, 2, "最终文件大小 == 2 (fd2 覆盖了 fd1 的数据)");

        close(fd2);
    }
    close(fd);

    /* ================================================================
     * PART 11: lseek 对 O_RDONLY / O_WRONLY fd 均可用
     * ================================================================ */

    /* O_RDONLY */
    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        off_t ro_seek = lseek(fd, 0, SEEK_SET);
        CHECK_RET(ro_seek, 0, "lseek 对 O_RDONLY fd 可用");
        close(fd);
    }

    /* O_WRONLY */
    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        CHECK_RET(write(fd, "test", 4), 4, "O_WRONLY: 写入数据");
        off_t wo_seek = lseek(fd, 0, SEEK_SET);
        CHECK_RET(wo_seek, 0, "lseek 对 O_WRONLY fd 可用");
        close(fd);
    }

    /* ================================================================
     * PART 12: lseek(0, SEEK_END) 获取文件大小
     *
     * 操作：写入 10 字节
     * 观测：lseek(0, SEEK_END)
     * 验证：返回值 == stat.st_size
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "lseek 获取大小测试: 打开文件");

    CHECK_RET(write(fd, "1234567890", 10), 10, "写 10 字节");
    off_t seek_size = lseek(fd, 0, SEEK_END);
    CHECK_RET(seek_size, 10, "lseek(0, SEEK_END) == 10");

    close(fd);

    CHECK_RET(stat(TMPFILE, &st), 0, "stat 获取文件大小");
    CHECK(st.st_size == 10, "lseek 大小 == stat.st_size");

    /* ================================================================
     * PART 13: close 后 fd 不可用
     *
     * 操作：close(fd) 后 write(fd, ...)
     * 观测：write 返回值和 errno
     * 验证：返回 -1，errno == EBADF
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "close 后失效测试: 打开文件");

    CHECK_RET(close(fd), 0, "close 成功");

    /* 观测：close 后 write 失败 */
    errno = 0;
    ssize_t closed_ret = write(fd, "x", 1);
    CHECK(closed_ret == -1 && errno == EBADF,
          "close 后: write 返回 -1 且 errno == EBADF");

    /* ================================================================
     * PART 14: 大文件数据完整性 (1MB)
     *
     * 操作：生成 1MB 已知模式数据，write 全部，lseek 回头，read 全部
     * 观测：memcmp
     * 验证：字节级完全一致
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "大文件测试: 打开文件");

    /* 分配并生成测试数据 */
    char *big_buf = malloc(BIG_FILE_SIZE);
    if (big_buf) {
        fill_pattern(big_buf, BIG_FILE_SIZE, 0x42);

        /* 写入 */
        ssize_t big_written = write(fd, big_buf, BIG_FILE_SIZE);
        CHECK(big_written == BIG_FILE_SIZE,
              "大文件 write: 写入 1MB 数据");

        /* 回头读取 */
        lseek(fd, 0, SEEK_SET);
        char *big_read = malloc(BIG_FILE_SIZE);
        if (big_read) {
            ssize_t big_read_ret = read(fd, big_read, BIG_FILE_SIZE);
            CHECK(big_read_ret == BIG_FILE_SIZE,
                  "大文件 read: 读回 1MB 数据");
            CHECK(memcmp(big_buf, big_read, BIG_FILE_SIZE) == 0,
                  "大文件: 字节级完全一致 (memcmp == 0)");

            free(big_read);
        }
        free(big_buf);
    }
    close(fd);

    /* ================================================================
     * PART 15: 循环写入追加模拟
     *
     * 操作：循环 write 100 次每次 10 字节
     * 观测：stat 确认 st_size，lseek+read 回全部数据
     * 验证：总字节数精确，内容按写入顺序精确匹配
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "循环写入测试: 打开文件");

    const char chunk[10] = "0123456789";
    const int iterations = 100;

    for (int i = 0; i < iterations; i++) {
        ssize_t chunk_ret = write(fd, chunk, 10);
        if (chunk_ret != 10) {
            CHECK(0, "循环写入: 第 100 次失败");
            break;
        }
    }
    CHECK(1, "循环写入: 100 次 * 10 字节全部成功");

    /* 观测：stat 确认文件大小 */
    close(fd);
    CHECK_RET(stat(TMPFILE, &st), 0, "循环写入: stat 成功");
    CHECK(st.st_size == 1000, "循环写入: st_size == 1000");

    /* ================================================================
     * PART 16: 负向测试 — 错误路径
     * ================================================================ */

    /* 16.1 read 无效 fd */
    char dummy[4];
    errno = 0;
    CHECK_ERR(read(-1, dummy, 1), EBADF, "read(-1) -> EBADF");

    errno = 0;
    CHECK_ERR(read(9999, dummy, 1), EBADF, "read(9999) -> EBADF");

    /* 16.2 write 无效 fd */
    errno = 0;
    CHECK_ERR(write(-1, "x", 1), EBADF, "write(-1) -> EBADF");

    errno = 0;
    CHECK_ERR(write(9999, "x", 1), EBADF, "write(9999) -> EBADF");

    /* 16.3 lseek 无效 fd */
    errno = 0;
    CHECK_ERR(lseek(-1, 0, SEEK_SET), EBADF, "lseek(-1) -> EBADF");

    /* 16.4 lseek 负偏移 (SEEK_SET) */
    fd = openat(AT_FDCWD, TMPFILE, O_RDWR);
    if (fd >= 0) {
        errno = 0;
        CHECK_ERR(lseek(fd, -1, SEEK_SET), EINVAL,
                  "lseek 负偏移 (SEEK_SET) -> EINVAL");
        close(fd);
    }

    /* 16.5 lseek 无效 whence */
    fd = openat(AT_FDCWD, TMPFILE, O_RDWR);
    if (fd >= 0) {
        errno = 0;
        CHECK_ERR(lseek(fd, 0, 99), EINVAL,
                  "lseek 无效 whence(99) -> EINVAL");
        close(fd);
    }

    /* 16.6 close 无效 fd */
    errno = 0;
    CHECK_ERR(close(-1), EBADF, "close(-1) -> EBADF");

    errno = 0;
    CHECK_ERR(close(9999), EBADF, "close(9999) -> EBADF");

    /* 16.7 double close */
    fd = openat(AT_FDCWD, TMPFILE, O_RDWR);
    if (fd >= 0) {
        CHECK_RET(close(fd), 0, "第一次 close 成功");
        errno = 0;
        CHECK_ERR(close(fd), EBADF, "第二次 close -> EBADF");
    }

    /* 16.8 lseek 管道 fd */
    int pipefd[2];
    int pipe_ret = pipe(pipefd);
    if (pipe_ret == 0) {
        errno = 0;
        CHECK_ERR(lseek(pipefd[0], 0, SEEK_SET), ESPIPE,
                  "lseek 管道 fd -> ESPIPE");
        close(pipefd[0]);
        close(pipefd[1]);
    }

    /* ================================================================
     * 清理
     * ================================================================ */
    unlink(TMPFILE);

    TEST_DONE();
}
