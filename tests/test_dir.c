/*
 * test_dir.c — 目录与路径操作完整测试
 *
 * 测试策略：验证 mkdirat/getcwd/chdir/unlinkat/rename/linkat/symlinkat/getdents64 语义
 *
 * 覆盖范围：
 *   正向：创建目录、切换目录、获取工作目录、删除文件/目录、
 *         重命名、硬链接、符号链接、读取目录条目
 *   负向：已存在目录 EEXIST、非空目录 ENOTEMPTY、不存在路径 ENOENT
 */

#include "test_framework.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#define BASEDIR "/tmp/starry_dir_test"
#define TMPFILE "/tmp/starry_dir_test/workfile"

static void cleanup(void)
{
    unlink(BASEDIR "/subdir/linktgt");
    unlink(BASEDIR "/subdir/renamed");
    unlink(BASEDIR "/subdir/hardlink");
    unlink(BASEDIR "/subdir/workfile");
    rmdir(BASEDIR "/subdir");
    rmdir(BASEDIR "/emptydir");
    rmdir(BASEDIR);
}

int main(void)
{
    TEST_START("dir: mkdirat/getcwd/chdir/unlinkat/rename/linkat/symlinkat/getdents64");

    cleanup();

    /* 创建基础目录 */
    CHECK_RET(mkdir(BASEDIR, 0755), 0, "mkdir 基础目录成功");

    /* ================================================================
     * PART 1: mkdirat — 创建目录
     * ================================================================ */

    {
        int dfd = open(BASEDIR, O_RDONLY | O_DIRECTORY);
        CHECK(dfd >= 0, "PART1: 打开基础目录 fd 成功");
        if (dfd < 0) { TEST_DONE(); }

        errno = 0;
        int ret = mkdirat(dfd, "emptydir", 0755);
        CHECK_RET(ret, 0, "PART1: mkdirat 创建 emptydir 成功");

        struct stat st;
        CHECK_RET(fstatat(dfd, "emptydir", &st, 0), 0,
                  "PART1: fstatat 获取 emptydir 信息成功");
        CHECK(S_ISDIR(st.st_mode), "PART1: emptydir 是目录 (S_ISDIR)");

        close(dfd);
    }

    /* ================================================================
     * PART 2: chdir + getcwd
     * ================================================================ */

    {
        char old_cwd[4096];
        CHECK(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "PART2: getcwd 获取初始工作目录成功");

        CHECK_RET(chdir(BASEDIR), 0, "PART2: chdir 到 BASEDIR 成功");

        char new_cwd[4096];
        CHECK(getcwd(new_cwd, sizeof(new_cwd)) != NULL,
              "PART2: chdir 后 getcwd 成功");
        CHECK(strcmp(new_cwd, BASEDIR) == 0,
              "PART2: getcwd 返回的路径与 BASEDIR 一致");

        chdir(old_cwd);
    }

    /* ================================================================
     * PART 3: unlinkat — 删除文件和目录
     * ================================================================ */

    {
        int dfd = open(BASEDIR, O_RDONLY | O_DIRECTORY);
        CHECK(dfd >= 0, "PART3: 打开基础目录 fd 成功");
        if (dfd < 0) { TEST_DONE(); }

        int fd = openat(dfd, "workfile", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        CHECK(fd >= 0, "PART3: 创建 workfile 成功");
        if (fd >= 0) {
            write(fd, "hello", 5);
            close(fd);
        }

        CHECK_RET(unlinkat(dfd, "workfile", 0), 0,
                  "PART3: unlinkat 删除文件成功");

        struct stat st;
        errno = 0;
        CHECK(fstatat(dfd, "workfile", &st, 0) == -1 && errno == ENOENT,
              "PART3: 删除后 fstatat 返回 ENOENT");

        CHECK_RET(unlinkat(dfd, "emptydir", AT_REMOVEDIR), 0,
                  "PART3: unlinkat AT_REMOVEDIR 删除空目录成功");

        errno = 0;
        CHECK(fstatat(dfd, "emptydir", &st, 0) == -1 && errno == ENOENT,
              "PART3: 目录删除后 fstatat 返回 ENOENT");

        close(dfd);
    }

    /* ================================================================
     * PART 4: rename — 重命名文件
     * ================================================================ */

    {
        int dfd = open(BASEDIR, O_RDONLY | O_DIRECTORY);
        CHECK(dfd >= 0, "PART4: 打开基础目录 fd 成功");
        if (dfd < 0) { TEST_DONE(); }

        int fd = openat(dfd, "srcfile", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        CHECK(fd >= 0, "PART4: 创建 srcfile 成功");
        if (fd >= 0) {
            write(fd, "rename_test", 11);
            close(fd);
        }

        CHECK_RET(renameat(dfd, "srcfile", dfd, "dstfile"), 0,
                  "PART4: renameat srcfile -> dstfile 成功");

        struct stat st;
        errno = 0;
        CHECK(fstatat(dfd, "srcfile", &st, 0) == -1 && errno == ENOENT,
              "PART4: 重命名后源文件不存在");

        CHECK_RET(fstatat(dfd, "dstfile", &st, 0), 0,
                  "PART4: 重命名后目标文件存在");
        CHECK(st.st_size == 11, "PART4: 目标文件大小 == 11 (内容保留)");

        unlinkat(dfd, "dstfile", 0);
        close(dfd);
    }

    /* ================================================================
     * PART 5: linkat — 硬链接
     * ================================================================ */

    {
        int dfd = open(BASEDIR, O_RDONLY | O_DIRECTORY);
        CHECK(dfd >= 0, "PART5: 打开基础目录 fd 成功");
        if (dfd < 0) { TEST_DONE(); }

        int fd = openat(dfd, "origfile", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        CHECK(fd >= 0, "PART5: 创建 origfile 成功");
        if (fd >= 0) {
            write(fd, "hardlink", 8);
            close(fd);
        }

        CHECK_RET(linkat(dfd, "origfile", dfd, "hardlink", 0), 0,
                  "PART5: linkat 创建硬链接成功");

        struct stat st1, st2;
        CHECK_RET(fstatat(dfd, "origfile", &st1, 0), 0,
                  "PART5: stat 原文件成功");
        CHECK_RET(fstatat(dfd, "hardlink", &st2, 0), 0,
                  "PART5: stat 硬链接成功");
        CHECK(st1.st_ino == st2.st_ino,
              "PART5: 硬链接与原文件 st_ino 一致");
        CHECK(st1.st_dev == st2.st_dev,
              "PART5: 硬链接与原文件 st_dev 一致");
        CHECK(st1.st_nlink == 2,
              "PART5: st_nlink == 2 (原文件 + 硬链接)");

        unlinkat(dfd, "origfile", 0);
        unlinkat(dfd, "hardlink", 0);
        close(dfd);
    }

    /* ================================================================
     * PART 6: symlinkat + readlink — 符号链接
     * ================================================================ */

    {
        int dfd = open(BASEDIR, O_RDONLY | O_DIRECTORY);
        CHECK(dfd >= 0, "PART6: 打开基础目录 fd 成功");
        if (dfd < 0) { TEST_DONE(); }

        int fd = openat(dfd, "realfile", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        CHECK(fd >= 0, "PART6: 创建 realfile 成功");
        if (fd >= 0) {
            write(fd, "symlink_data", 13);
            close(fd);
        }

        CHECK_RET(symlinkat("realfile", dfd, "symlink"), 0,
                  "PART6: symlinkat 创建符号链接成功");

        struct stat st;
        CHECK_RET(fstatat(dfd, "symlink", &st, 0), 0,
                  "PART6: fstatat 跟随符号链接 stat 成功");
        CHECK(S_ISREG(st.st_mode),
              "PART6: fstatat 跟随符号链接看到普通文件");

        CHECK_RET(fstatat(dfd, "symlink", &st, AT_SYMLINK_NOFOLLOW), 0,
                  "PART6: fstatat AT_SYMLINK_NOFOLLOW 成功");
        CHECK(S_ISLNK(st.st_mode),
              "PART6: AT_SYMLINK_NOFOLLOW 看到符号链接");

        char linkbuf[256];
        ssize_t linklen = readlinkat(dfd, "symlink", linkbuf, sizeof(linkbuf) - 1);
        CHECK(linklen > 0, "PART6: readlinkat 返回正长度");
        if (linklen > 0) {
            linkbuf[linklen] = '\0';
            CHECK(strcmp(linkbuf, "realfile") == 0,
                  "PART6: readlinkat 返回的目标路径正确");
        }

        unlinkat(dfd, "symlink", 0);
        unlinkat(dfd, "realfile", 0);
        close(dfd);
    }

    /* ================================================================
     * PART 7: getdents64 — 读取目录条目
     * ================================================================ */

    {
        mkdir(BASEDIR "/testdir", 0755);
        {
            int fd = open(BASEDIR "/testdir/testfile", O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd >= 0) close(fd);
        }

        int dfd = open(BASEDIR "/testdir", O_RDONLY | O_DIRECTORY);
        CHECK(dfd >= 0, "PART7: 打开 testdir fd 成功");
        if (dfd < 0) { TEST_DONE(); }

        char buf[4096];
        memset(buf, 0, sizeof(buf));
        int nread = (int)syscall(SYS_getdents64, dfd, buf, sizeof(buf));
        CHECK(nread > 0, "PART7: getdents64 返回正长度");

        int found_self = 0, found_parent = 0, found_file = 0;
        long pos = 0;
        while (pos < nread) {
            struct linux_dirent64 {
                uint64_t d_ino;
                int64_t d_off;
                unsigned short d_reclen;
                unsigned char d_type;
                char d_name[];
            };
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);
            if (strcmp(d->d_name, ".") == 0) found_self = 1;
            if (strcmp(d->d_name, "..") == 0) found_parent = 1;
            if (strcmp(d->d_name, "testfile") == 0) found_file = 1;
            pos += d->d_reclen;
        }

        CHECK(found_self, "PART7: getdents64 找到 '.' 条目");
        CHECK(found_parent, "PART7: getdents64 找到 '..' 条目");
        CHECK(found_file, "PART7: getdents64 找到 'testfile' 条目");

        close(dfd);
        unlink(BASEDIR "/testdir/testfile");
        rmdir(BASEDIR "/testdir");
    }

    /* ================================================================
     * PART 8: 负向测试
     * ================================================================ */

    {
        int dfd = open(BASEDIR, O_RDONLY | O_DIRECTORY);
        CHECK(dfd >= 0, "PART8: 打开基础目录 fd 成功");

        mkdir(BASEDIR "/existing", 0755);

        errno = 0;
        CHECK(mkdirat(dfd, "existing", 0755) == -1 && errno == EEXIST,
              "PART8: mkdirat 已存在目录返回 EEXIST");

        mkdir(BASEDIR "/nonempty", 0755);
        {
            int fd = open(BASEDIR "/nonempty/child", O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd >= 0) close(fd);
        }

        errno = 0;
        CHECK(unlinkat(dfd, "nonempty", AT_REMOVEDIR) == -1 && errno == ENOTEMPTY,
              "PART8: unlinkat 非空目录返回 ENOTEMPTY");

        errno = 0;
        CHECK(chdir("/tmp/starry_dir_test_no_such_path") == -1 && errno == ENOENT,
              "PART8: chdir 不存在路径返回 ENOENT");

        errno = 0;
        CHECK(unlinkat(dfd, "no_such_file", 0) == -1 && errno == ENOENT,
              "PART8: unlinkat 不存在文件返回 ENOENT");

        unlink(BASEDIR "/nonempty/child");
        rmdir(BASEDIR "/nonempty");
        rmdir(BASEDIR "/existing");
        close(dfd);
    }

    /* ================================================================
     * PART 9: rename 跨目录
     * ================================================================ */

    {
        mkdir(BASEDIR "/dir_a", 0755);
        mkdir(BASEDIR "/dir_b", 0755);

        int fd = open(BASEDIR "/dir_a/movefile", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        CHECK(fd >= 0, "PART9: 创建 dir_a/movefile");
        if (fd >= 0) {
            write(fd, "moved", 5);
            close(fd);
        }

        int dfd_a = open(BASEDIR "/dir_a", O_RDONLY | O_DIRECTORY);
        int dfd_b = open(BASEDIR "/dir_b", O_RDONLY | O_DIRECTORY);
        CHECK(dfd_a >= 0 && dfd_b >= 0, "PART9: 打开 dir_a 和 dir_b fd");

        if (dfd_a >= 0 && dfd_b >= 0) {
            CHECK_RET(renameat(dfd_a, "movefile", dfd_b, "movedfile"), 0,
                      "PART9: renameat 跨目录移动成功");

            struct stat st;
            errno = 0;
            CHECK(fstatat(dfd_a, "movefile", &st, 0) == -1 && errno == ENOENT,
                  "PART9: 移动后源文件不存在");

            CHECK_RET(fstatat(dfd_b, "movedfile", &st, 0), 0,
                      "PART9: 移动后目标文件存在");
            CHECK(st.st_size == 5, "PART9: 移动后文件大小 == 5");

            unlinkat(dfd_b, "movedfile", 0);
        }

        if (dfd_a >= 0) close(dfd_a);
        if (dfd_b >= 0) close(dfd_b);
        rmdir(BASEDIR "/dir_a");
        rmdir(BASEDIR "/dir_b");
    }

    cleanup();

    TEST_DONE();
}
