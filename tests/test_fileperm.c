/*
 * test_fileperm.c — 文件权限与 umask 完整测试
 *
 * 测试策略：验证 chmod/fchmod/faccessat/umask 语义
 *
 * 覆盖范围：
 *   正向：chmod 改变文件 mode、fchmod 通过 fd 改变 mode、
 *         umask 影响创建文件 mode、faccessat 检查文件权限
 *   负向：chmod 不存在文件 ENOENT、fchmod 无效 fd EBADF、
 *         faccessat 不存在文件 ENOENT
 */

#include "test_framework.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

#define TMPFILE "/tmp/starry_fileperm_test"

int main(void)
{
    TEST_START("fileperm: chmod/fchmod/faccessat/umask");

    unlink(TMPFILE);

    /* ================================================================
     * PART 1: chmod — 改变文件 mode
     * ================================================================ */

    {
        int fd = open(TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(fd >= 0, "PART1: 创建测试文件成功");
        if (fd < 0) { TEST_DONE(); }
        close(fd);

        CHECK_RET(chmod(TMPFILE, 0755), 0, "PART1: chmod 0755 成功");

        struct stat st;
        CHECK_RET(stat(TMPFILE, &st), 0, "PART1: stat 获取文件信息成功");
        CHECK((st.st_mode & 07777) == 0755, "PART1: st_mode & 07777 == 0755");

        CHECK_RET(chmod(TMPFILE, 0600), 0, "PART1: chmod 0600 成功");
        CHECK_RET(stat(TMPFILE, &st), 0, "PART1: stat 获取文件信息成功");
        CHECK((st.st_mode & 07777) == 0600, "PART1: st_mode & 07777 == 0600");

        CHECK_RET(chmod(TMPFILE, 0777), 0, "PART1: chmod 0777 成功");
        CHECK_RET(stat(TMPFILE, &st), 0, "PART1: stat 获取文件信息成功");
        CHECK((st.st_mode & 07777) == 0777, "PART1: st_mode & 07777 == 0777");

        unlink(TMPFILE);
    }

    /* ================================================================
     * PART 2: fchmod — 通过 fd 改变 mode
     * ================================================================ */

    {
        int fd = open(TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(fd >= 0, "PART2: 创建测试文件成功");
        if (fd < 0) { TEST_DONE(); }

        CHECK_RET(fchmod(fd, 0755), 0, "PART2: fchmod 0755 成功");

        struct stat st;
        CHECK_RET(fstat(fd, &st), 0, "PART2: fstat 成功");
        CHECK((st.st_mode & 07777) == 0755, "PART2: fstat st_mode & 07777 == 0755");

        CHECK_RET(fchmod(fd, 0444), 0, "PART2: fchmod 0444 成功");
        CHECK_RET(fstat(fd, &st), 0, "PART2: fstat 成功");
        CHECK((st.st_mode & 07777) == 0444, "PART2: fstat st_mode & 07777 == 0444");

        close(fd);
        unlink(TMPFILE);
    }

    /* ================================================================
     * PART 3: umask — 影响文件创建 mode
     * ================================================================ */

    {
        mode_t old_mask = umask(0022);
        CHECK(old_mask != (mode_t)-1, "PART3: umask(0022) 返回旧值");

        int fd = open(TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0666);
        CHECK(fd >= 0, "PART3: 创建文件 0666 with umask 0022");
        if (fd >= 0) {
            struct stat st;
            CHECK_RET(fstat(fd, &st), 0, "PART3: fstat 成功");
            CHECK((st.st_mode & 07777) == 0644,
                  "PART3: 实际 mode == 0644 (0666 & ~0022)");
            close(fd);
        }
        unlink(TMPFILE);

        umask(0077);
        fd = open(TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0666);
        CHECK(fd >= 0, "PART3: 创建文件 0666 with umask 0077");
        if (fd >= 0) {
            struct stat st;
            CHECK_RET(fstat(fd, &st), 0, "PART3: fstat 成功");
            CHECK((st.st_mode & 07777) == 0600,
                  "PART3: 实际 mode == 0600 (0666 & ~0077)");
            close(fd);
        }
        unlink(TMPFILE);

        umask(0000);
        fd = open(TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0666);
        CHECK(fd >= 0, "PART3: 创建文件 0666 with umask 0000");
        if (fd >= 0) {
            struct stat st;
            CHECK_RET(fstat(fd, &st), 0, "PART3: fstat 成功");
            CHECK((st.st_mode & 07777) == 0666,
                  "PART3: 实际 mode == 0666 (umask 0000 不屏蔽)");
            close(fd);
        }
        unlink(TMPFILE);

        umask(old_mask);
    }

    /* ================================================================
     * PART 4: umask 恢复 — 设置/获取返回旧值
     * ================================================================ */

    {
        mode_t m1 = umask(0013);
        mode_t m2 = umask(0013);
        CHECK(m2 == 0013, "PART4: umask 返回上一次设置的值 0013");

        mode_t m3 = umask(m1);
        CHECK(m3 == 0013, "PART4: 恢复后 umask 返回 0013");
    }

    /* ================================================================
     * PART 5: faccessat — 检查文件可访问性
     * ================================================================ */

    {
        int fd = open(TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0755);
        CHECK(fd >= 0, "PART5: 创建测试文件 0755");
        if (fd < 0) { TEST_DONE(); }
        close(fd);

        int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
        CHECK(dfd >= 0, "PART5: 打开 /tmp 目录 fd");
        if (dfd < 0) { TEST_DONE(); }

        const char *relpath = TMPFILE + strlen("/tmp/");

        CHECK_RET(faccessat(dfd, relpath, F_OK, 0), 0,
                  "PART5: faccessat F_OK 文件存在");

        CHECK_RET(faccessat(dfd, relpath, R_OK, 0), 0,
                  "PART5: faccessat R_OK 文件可读");

        CHECK_RET(faccessat(dfd, relpath, W_OK, 0), 0,
                  "PART5: faccessat W_OK 文件可写");

        CHECK_RET(faccessat(dfd, relpath, X_OK, 0), 0,
                  "PART5: faccessat X_OK 文件可执行");

        CHECK_RET(faccessat(AT_FDCWD, TMPFILE, F_OK, 0), 0,
                  "PART5: faccessat AT_FDCWD + 绝对路径成功");

        unlink(TMPFILE);

        errno = 0;
        CHECK(faccessat(dfd, "starry_no_such_file_test", F_OK, 0) == -1 &&
              errno == ENOENT,
              "PART5: faccessat 不存在文件返回 ENOENT");

        close(dfd);
    }

    /* ================================================================
     * PART 6: faccessat — 无权限文件
     * ================================================================ */

    {
        int fd = open(TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0000);
        CHECK(fd >= 0, "PART6: 创建无权限文件 0000");
        if (fd < 0) { TEST_DONE(); }
        close(fd);

        int is_root = (geteuid() == 0);

        if (is_root) {
            CHECK_RET(access(TMPFILE, R_OK), 0,
                      "PART6: root 绕过权限检查 access R_OK 成功");
        } else {
            errno = 0;
            CHECK(access(TMPFILE, R_OK) == -1 && errno == EACCES,
                  "PART6: 无权限文件 access R_OK 返回 EACCES");
        }

        chmod(TMPFILE, 0644);
        unlink(TMPFILE);
    }

    /* ================================================================
     * PART 7: fchmodat — 通过目录 fd 改变文件 mode
     * ================================================================ */

    {
        int fd = open(TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(fd >= 0, "PART7: 创建测试文件");
        if (fd >= 0) close(fd);

        int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
        CHECK(dfd >= 0, "PART7: 打开 /tmp 目录 fd");
        if (dfd < 0) { TEST_DONE(); }

        const char *relpath = TMPFILE + strlen("/tmp/");

        CHECK_RET(fchmodat(dfd, relpath, 0755, 0), 0,
                  "PART7: fchmodat 0755 成功");

        struct stat st;
        CHECK_RET(stat(TMPFILE, &st), 0, "PART7: stat 成功");
        CHECK((st.st_mode & 07777) == 0755, "PART7: mode == 0755");

        CHECK_RET(fchmodat(AT_FDCWD, TMPFILE, 0600, 0), 0,
                  "PART7: fchmodat AT_FDCWD 成功");

        close(dfd);
        unlink(TMPFILE);
    }

    /* ================================================================
     * PART 8: 负向测试
     * ================================================================ */

    {
        errno = 0;
        CHECK(chmod("/tmp/starry_no_such_file_perm_test", 0644) == -1 &&
              errno == ENOENT,
              "PART8: chmod 不存在文件返回 ENOENT");

        errno = 0;
        CHECK(fchmod(-1, 0644) == -1 && errno == EBADF,
              "PART8: fchmod 无效 fd 返回 EBADF");

        errno = 0;
        CHECK(fchmod(9999, 0644) == -1 && errno == EBADF,
              "PART8: fchmod 未打开 fd 返回 EBADF");

        errno = 0;
        CHECK(faccessat(AT_FDCWD, "/tmp/starry_no_such_perm_test", F_OK, 0) == -1 &&
              errno == ENOENT,
              "PART8: faccessat 不存在文件返回 ENOENT");
    }

    unlink(TMPFILE);

    TEST_DONE();
}
