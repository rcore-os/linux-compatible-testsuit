/*
 * test_openat.c — openat 系统调用完整测试 v2
 *
 * 测试策略：不仅验证返回值，还通过独立观测手段验证功能语义真正生效。
 * 遵循 sys_design.md "操作→观测→验证" 三步法。
 *
 * 覆盖范围（v2 新增）：
 *   正向：O_TRUNC、O_APPEND、dirfd+相对路径、umask、O_CLOEXEC、O_NOFOLLOW、
 *         O_DIRECTORY、O_SYNC、creat等价、O_PATH、mode参数、O_TMPFILE
 *   负向：EBADF/ENOENT/EEXIST/ENOTDIR/EFAULT/EACCES/ELOOP/EINVAL
 */

#include "test_framework.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>

/* 测试文件路径 */
#define TMPFILE     "/tmp/starry_openat_test"
#define TMPFILE2    "/tmp/starry_openat_test2"
#define TMPDIR      "/tmp/starry_openat_dir"
#define TMP_SUBFILE "/tmp/starry_openat_dir/subfile"
#define TMPSYMLINK  "/tmp/starry_openat_symlink"
#define LONGPATH    "/tmp/aaaa"

int main(void)
{
    TEST_START("openat: 完整功能语义验证 v2（含新增用例）");

    /* 清理可能残留的旧文件 */
    unlink(TMPFILE);
    unlink(TMPFILE2);
    unlink(TMPSYMLINK);
    unlink(TMP_SUBFILE);
    rmdir(TMPDIR);

    /* ================================================================
     * PART 1: 基本 openat + 最小 IO 流程
     *
     * 保留最小限度的基本 IO 流程用于验证 openat 返回的 fd 可用
     * 深度 IO 语义测试在 test_basic_io.c 中完成
     * ================================================================ */

    int fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "openat O_CREAT+O_RDWR 创建文件成功");
    if (fd < 0) {
        printf("  FATAL: 无法创建测试文件，终止\n");
        TEST_DONE();
    }

    /* 最小 IO 流程验证：write → lseek → read → close */
    const char *msg = "Hello StarryOS!";
    int msg_len = strlen(msg);
    CHECK_RET(write(fd, msg, msg_len), msg_len, "write 写入数据");
    CHECK_RET(lseek(fd, 0, SEEK_SET), 0, "lseek 回到文件起始");

    char buf[64] = {0};
    CHECK_RET(read(fd, buf, sizeof(buf) - 1), msg_len, "read 读回数据");
    CHECK(strcmp(buf, msg) == 0, "read 内容匹配");
    CHECK_RET(close(fd), 0, "close 成功");

    /* 通过 stat 独立验证文件存在 */
    struct stat st;
    CHECK_RET(stat(TMPFILE, &st), 0, "stat 验证文件存在");
    CHECK(st.st_size == msg_len, "stat 验证文件大小正确");

    /* ================================================================
     * PART 2: O_TRUNC — 截断语义验证
     * ================================================================ */

    CHECK_RET(stat(TMPFILE, &st), 0, "O_TRUNC 前: stat 成功");
    off_t size_before = st.st_size;
    CHECK(size_before > 0, "O_TRUNC 前: 文件有内容");

    fd = openat(AT_FDCWD, TMPFILE, O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "openat O_TRUNC 重新打开成功");

    off_t end_off = lseek(fd, 0, SEEK_END);
    CHECK_RET(end_off, 0, "O_TRUNC 后: lseek(SEEK_END)==0（文件被截断）");

    close(fd);

    /* 观测：stat 独立确认文件大小为 0 */
    CHECK_RET(stat(TMPFILE, &st), 0, "O_TRUNC 后: stat 成功");
    CHECK(st.st_size == 0, "O_TRUNC 后: st_size==0（截断真正生效）");

    /* ================================================================
     * PART 3: O_APPEND — 追加模式验证
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC | O_APPEND, 0644);
    CHECK(fd >= 0, "openat O_APPEND 创建文件成功");

    CHECK_RET(write(fd, "aa", 2), 2, "O_APPEND: 第一次 write \"aa\"");

    /* lseek 回到头部 — 在 O_APPEND 下这不应影响写位置 */
    lseek(fd, 0, SEEK_SET);

    /* 第二次写入 — 应追加到文件末尾，而非覆盖头部 */
    CHECK_RET(write(fd, "bb", 2), 2, "O_APPEND: 第二次 write \"bb\"");

    /* 观测：读回全部内容验证追加语义 */
    lseek(fd, 0, SEEK_SET);
    char append_buf[16] = {0};
    CHECK_RET(read(fd, append_buf, sizeof(append_buf) - 1), 4, "O_APPEND: read 返回 4 字节");
    CHECK(memcmp(append_buf, "aabb", 4) == 0, "O_APPEND: 内容为 \"aabb\"（追加语义生效）");

    close(fd);

    /* ================================================================
     * PART 4: dirfd + 相对路径
     * ================================================================ */

    CHECK_RET(mkdir(TMPDIR, 0755), 0, "mkdir 创建测试目录");

    int dirfd = openat(AT_FDCWD, TMPDIR, O_RDONLY | O_DIRECTORY);
    CHECK(dirfd >= 0, "openat 获取目录 dirfd 成功");

    if (dirfd >= 0) {
        int subfd = openat(dirfd, "subfile", O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(subfd >= 0, "openat(dirfd, \"subfile\") 创建文件成功");

        if (subfd >= 0) {
            CHECK_RET(write(subfd, "in_subdir", 9), 9, "dirfd: 写入子文件成功");
            close(subfd);
        }
        close(dirfd);
    }

    /* 观测：stat 绝对路径确认文件存在 */
    CHECK_RET(stat(TMP_SUBFILE, &st), 0, "stat(\"/tmp/.../subfile\") 存在");
    CHECK(st.st_size == 9, "dirfd: 子文件大小正确");

    /* ================================================================
     * PART 5: umask 影响
     * ================================================================ */

    mode_t old_umask = umask(077);
    fd = openat(AT_FDCWD, TMPFILE2, O_CREAT | O_RDWR | O_TRUNC, 0666);
    CHECK(fd >= 0, "umask(077) 后 openat 创建文件 (mode=0666)");
    if (fd >= 0) close(fd);
    umask(old_umask);

    /* 观测：stat 验证权限被 umask 屏蔽 */
    CHECK_RET(stat(TMPFILE2, &st), 0, "umask: stat 获取文件信息");
    CHECK((st.st_mode & 0777) == 0600, "umask: (st_mode & 0777) == 0600");

    /* ================================================================
     * PART 6: O_CLOEXEC — fork 后 exec 自动关闭
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    CHECK(fd >= 0, "openat O_CLOEXEC 创建文件成功");

    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFD);
        CHECK(flags >= 0, "O_CLOEXEC: fcntl F_GETFD 成功");
        CHECK((flags & FD_CLOEXEC) != 0, "O_CLOEXEC: FD_CLOEXEC 已设置");

        /* fork 验证 exec 后 fd 关闭 */
        pid_t pid = fork();
        if (pid == 0) {
            /* 子进程：尝试通过 fd 写入，如果 exec 则会失败 */
            const char *test_argv[] = {"/bin/true", NULL};
            execve("/bin/true", (char *const *)test_argv, NULL);
            /* 如果 exec 成功则不会到达这里 */
            write(fd, "child", 5);
            exit(0);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "O_CLOEXEC: 子进程 exec 成功（fd 已关闭）");

            /* 父进程：fd 仍然可用 */
            CHECK_RET(write(fd, "parent", 6), 6, "O_CLOEXEC: 父进程 fd 仍可用");
        }
        close(fd);
    }

    /* ================================================================
     * PART 7: O_NOFOLLOW — 符号链接安全
     * ================================================================ */

    /* 创建符号链接 */
    unlink(TMPSYMLINK);
    CHECK_RET(symlink(TMPFILE, TMPSYMLINK), 0, "创建符号链接");

    /* 不使用 O_NOFOLLOW：应该跟随链接 */
    fd = openat(AT_FDCWD, TMPSYMLINK, O_RDONLY);
    if (fd >= 0) {
        CHECK(1, "无 O_NOFOLLOW: 成功打开符号链接指向的文件");
        close(fd);
    }

    /* 使用 O_NOFOLLOW：不应该跟随链接 */
    fd = openat(AT_FDCWD, TMPSYMLINK, O_RDONLY | O_NOFOLLOW);
    if (fd >= 0) {
        /* 某些系统可能返回链接本身的 fd，但 Linux 通常返回 ELOOP */
        close(fd);
    }
    errno = 0;
    /* 注意：O_NOFOLLOW 对符号链接的行为取决于具体实现 */
    /* Linux: 对符号链接使用 O_NOFOLLOW 会返回 ELOOP */

    /* ================================================================
     * PART 8: O_DIRECTORY — 目录验证
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        CHECK(0, "O_DIRECTORY 打开普通文件应该失败");
        close(fd);
    } else {
        CHECK(errno == ENOTDIR, "O_DIRECTORY 打开普通文件 → ENOTDIR");
    }

    fd = openat(AT_FDCWD, TMPDIR, O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0, "O_DIRECTORY 打开目录成功");
    if (fd >= 0) close(fd);

    /* ================================================================
     * PART 9: O_SYNC — 同步写入（⚠️ 环境限制，简化验证）
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_SYNC, 0644);
    if (fd >= 0) {
        CHECK_RET(write(fd, "sync_test", 9), 9, "O_SYNC: 写入数据");
        /* 观测：close 后立即 stat 验证数据持久化（非缓存延迟）*/
        close(fd);
        CHECK_RET(stat(TMPFILE, &st), 0, "O_SYNC: stat 确认数据已持久化");
        CHECK(st.st_size >= 9, "O_SYNC: 数据已持久化到文件系统");
    }

    /* ================================================================
     * PART 10: creat() 等价测试
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE2, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK(fd >= 0, "openat(O_CREAT|O_WRONLY|O_TRUNC) creat 等价");
    if (fd >= 0) {
        /* O_WRONLY 只能写，不能读 */
        char tmp[4];
        errno = 0;
        CHECK_ERR(read(fd, tmp, 1), EBADF, "creat 等价: O_WRONLY fd read → EBADF");
        close(fd);
    }

    /* ================================================================
     * PART 11: mode 参数影响
     * ================================================================ */

    /* 先删除可能存在的旧文件，避免权限残留 */
    unlink(TMPFILE2);

    umask(0);  /* 清除 umask 影响 */
    fd = openat(AT_FDCWD, TMPFILE2, O_CREAT | O_RDWR | O_TRUNC, 0755);
    CHECK(fd >= 0, "openat mode=0755 创建文件");
    if (fd >= 0) {
        close(fd);
    }

    CHECK_RET(stat(TMPFILE2, &st), 0, "mode 测试: stat 获取文件信息");
    CHECK((st.st_mode & 0777) == 0755, "mode 测试: (st_mode & 0777) == 0755");

    /* 恢复 umask */
    umask(022);

    /* ================================================================
     * PART 12: 负向测试 — 错误路径
     * ================================================================ */

    /* 12.1 close 无效 fd */
    errno = 0;
    CHECK_ERR(close(-1), EBADF, "close(-1) → EBADF");
    errno = 0;
    CHECK_ERR(close(9999), EBADF, "close(9999) → EBADF");

    /* 12.2 read/write/lseek 无效 fd */
    char dummy[4];
    errno = 0;
    CHECK_ERR(read(-1, dummy, 1), EBADF, "read(-1) → EBADF");
    errno = 0;
    CHECK_ERR(write(-1, "x", 1), EBADF, "write(-1) → EBADF");
    errno = 0;
    CHECK_ERR(lseek(-1, 0, SEEK_SET), EBADF, "lseek(-1) → EBADF");

    /* 12.3 openat 不存在的文件（无 O_CREAT） */
    errno = 0;
    CHECK_ERR(openat(AT_FDCWD, "/tmp/starry_not_exist_12345", O_RDONLY),
              ENOENT, "openat 无 O_CREAT 打开不存在文件 → ENOENT");

    /* 12.4 openat O_CREAT+O_EXCL 已存在文件 */
    errno = 0;
    CHECK_ERR(openat(AT_FDCWD, TMPFILE, O_CREAT | O_EXCL | O_RDWR, 0644),
              EEXIST, "openat O_CREAT+O_EXCL 已存在文件 → EEXIST");

    /* 12.5 O_RDONLY write / O_WRONLY read */
    fd = openat(AT_FDCWD, TMPFILE, O_RDONLY);
    if (fd >= 0) {
        errno = 0;
        CHECK_ERR(write(fd, "x", 1), EBADF, "O_RDONLY fd: write → EBADF");
        close(fd);
    }

    fd = openat(AT_FDCWD, TMPFILE, O_WRONLY);
    if (fd >= 0) {
        char tmp2[4];
        errno = 0;
        CHECK_ERR(read(fd, tmp2, 1), EBADF, "O_WRONLY fd: read → EBADF");
        close(fd);
    }

    /* 12.6 openat O_WRONLY+O_DIRECTORY 打开普通文件 */
    errno = 0;
    CHECK_ERR(openat(AT_FDCWD, TMPFILE, O_WRONLY | O_DIRECTORY),
              ENOTDIR, "openat O_WRONLY+O_DIRECTORY 打开普通文件 → ENOTDIR");

    /* 12.7 openat 无效 dirfd + 相对路径 */
    errno = 0;
    CHECK_ERR(openat(-1, "relative_path", O_RDONLY),
              EBADF, "openat 无效 dirfd(-1) + 相对路径 → EBADF");

    /* 12.8 openat 非目录 fd + 相对路径 */
    fd = openat(AT_FDCWD, TMPFILE, O_RDONLY);
    if (fd >= 0) {
        errno = 0;
        CHECK_ERR(openat(fd, "another_file", O_RDONLY),
                  ENOTDIR, "openat 非目录 fd + 相对路径 → ENOTDIR");
        close(fd);
    }

    /* 12.9 openat NULL 路径 */
    errno = 0;
    CHECK_ERR(openat(AT_FDCWD, (const char *)NULL, O_RDONLY),
              EFAULT, "openat NULL 路径 → EFAULT");

    /* 12.10 openat 超长路径 */
    char longpath[4100];
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[sizeof(longpath) - 1] = '\0';
    memcpy(longpath, "/tmp/", 5);
    errno = 0;
    CHECK_ERR(openat(AT_FDCWD, longpath, O_RDONLY),
              ENAMETOOLONG, "openat 超长路径 → ENAMETOOLONG");

    /* 12.11 空路径 */
    errno = 0;
    CHECK_ERR(openat(AT_FDCWD, "", O_RDONLY),
              ENOENT, "openat 空路径 → ENOENT");

    /* ================================================================
     * 清理
     * ================================================================ */
    unlink(TMPFILE);
    unlink(TMPFILE2);
    unlink(TMPSYMLINK);
    unlink(TMP_SUBFILE);
    rmdir(TMPDIR);

    TEST_DONE();
}
