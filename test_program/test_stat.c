#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>

/* ---------------------------------------------------------------------------
 * Custom assertion macro: prints file/line/expected/actual on failure
 * --------------------------------------------------------------------------- */
#define TEST_ASSERT(expr, expected, actual)                                    \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr,                                                    \
                    "FAIL %s:%d: expected %jd, got %jd (%s)\n",               \
                    __FILE__, __LINE__,                                        \
                    (intmax_t)(expected), (intmax_t)(actual), #expr);          \
            abort();                                                           \
        }                                                                      \
    } while (0)

#define ASSERT_OK(call)                                                        \
    do {                                                                       \
        int _rc = (call);                                                      \
        if (_rc != 0) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s returned %d (errno=%d: %s)\n",    \
                    __FILE__, __LINE__, #call, _rc, errno, strerror(errno));   \
            abort();                                                           \
        }                                                                      \
    } while (0)

#define ASSERT_ERR(call, expected_errno)                                       \
    do {                                                                       \
        errno = 0;                                                             \
        int _rc = (call);                                                      \
        int _saved = errno;                                                    \
        if (_rc != -1) {                                                       \
            fprintf(stderr,                                                    \
                    "FAIL %s:%d: %s returned %d, expected -1 (errno=%d)\n",    \
                    __FILE__, __LINE__, #call, _rc, expected_errno);           \
            abort();                                                           \
        }                                                                      \
        if (_saved != (expected_errno)) {                                      \
            fprintf(stderr,                                                    \
                    "FAIL %s:%d: %s errno=%d (%s), expected %d (%s)\n",        \
                    __FILE__, __LINE__, #call,                                 \
                    _saved, strerror(_saved),                                  \
                    expected_errno, strerror(expected_errno));                 \
            abort();                                                           \
        }                                                                      \
    } while (0)

static int test_count = 0;
static int test_pass  = 0;

#define RUN_TEST(fn)                                                           \
    do {                                                                       \
        test_count++;                                                          \
        printf("  [%02d] %-50s ", test_count, #fn);                           \
        fflush(stdout);                                                        \
        fn();                                                                  \
        test_pass++;                                                           \
        printf("PASS\n");                                                      \
    } while (0)

/* ===========================================================================
 * Helper: create temporary files/dirs/links for testing
 * =========================================================================== */
static const char *TMPDIR = "/tmp/stat_test_XXXXXX";
static char basedir[256];

static void setup(void)
{
    snprintf(basedir, sizeof(basedir), "%s", TMPDIR);
    assert(mkdtemp(basedir) != NULL);

    char path[512];

    /* Regular file with known content */
    snprintf(path, sizeof(path), "%s/regfile.txt", basedir);
    {
        int fd = open(path, O_CREAT | O_WRONLY, 0644);
        assert(fd >= 0);
        const char *msg = "Hello, stat!";
        assert(write(fd, msg, strlen(msg)) == (ssize_t)strlen(msg));
        close(fd);
    }

    /* Sub-directory */
    snprintf(path, sizeof(path), "%s/subdir", basedir);
    assert(mkdir(path, 0755) == 0);

    /* Symlink pointing to the regular file */
    snprintf(path, sizeof(path), "%s/link_to_regfile", basedir);
    {
        char target[512];
        snprintf(target, sizeof(target), "%s/regfile.txt", basedir);
        assert(symlink(target, path) == 0);
    }

    /* Dangling symlink */
    snprintf(path, sizeof(path), "%s/dangling_link", basedir);
    assert(symlink("/tmp/no_such_file_ever_12345", path) == 0);

    /* FIFO (named pipe) */
    snprintf(path, sizeof(path), "%s/test_fifo", basedir);
    assert(mkfifo(path, 0644) == 0);
}

static void teardown(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", basedir);
    system(cmd);
}

/* ===========================================================================
 * Normal path tests
 * =========================================================================== */

/* Test 1: stat() on a regular file */
static void test_stat_regular_file(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/regfile.txt", basedir);
    struct stat sb;
    ASSERT_OK(stat(path, &sb));

    TEST_ASSERT(S_ISREG(sb.st_mode), 1, S_ISREG(sb.st_mode));
    TEST_ASSERT(sb.st_size == 12, 12, (intmax_t)sb.st_size); /* "Hello, stat!" */
    TEST_ASSERT(sb.st_nlink >= 1, 1, (intmax_t)sb.st_nlink);
    TEST_ASSERT(sb.st_uid == getuid(), getuid(), (intmax_t)sb.st_uid);
    TEST_ASSERT(sb.st_gid == getgid(), getgid(), (intmax_t)sb.st_gid);
    TEST_ASSERT(sb.st_blksize > 0, 1, (intmax_t)sb.st_blksize);
    TEST_ASSERT(sb.st_blocks >= 0, 0, (intmax_t)sb.st_blocks);
    TEST_ASSERT(sb.st_ino > 0, 1, (intmax_t)sb.st_ino);
}

/* Test 2: stat() on a directory */
static void test_stat_directory(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/subdir", basedir);
    struct stat sb;
    ASSERT_OK(stat(path, &sb));

    TEST_ASSERT(S_ISDIR(sb.st_mode), 1, S_ISDIR(sb.st_mode));
    TEST_ASSERT(sb.st_nlink >= 2, 2, (intmax_t)sb.st_nlink); /* "." and ".." */
}

/* Test 3: stat() on a symlink — should follow the link */
static void test_stat_symlink_follows(void)
{
    char linkpath[512], filepath[512];
    snprintf(linkpath, sizeof(linkpath), "%s/link_to_regfile", basedir);
    snprintf(filepath, sizeof(filepath), "%s/regfile.txt", basedir);

    struct stat sb_link, sb_file;
    ASSERT_OK(stat(linkpath, &sb_link));
    ASSERT_OK(stat(filepath, &sb_file));

    /* stat() follows the symlink, so both should give the same inode */
    TEST_ASSERT(sb_link.st_ino == sb_file.st_ino,
                (intmax_t)sb_file.st_ino, (intmax_t)sb_link.st_ino);
    TEST_ASSERT(S_ISREG(sb_link.st_mode), 1, S_ISREG(sb_link.st_mode));
}

/* Test 4: lstat() on a symlink — should NOT follow */
static void test_lstat_symlink_no_follow(void)
{
    char linkpath[512], filepath[512];
    snprintf(linkpath, sizeof(linkpath), "%s/link_to_regfile", basedir);
    snprintf(filepath, sizeof(filepath), "%s/regfile.txt", basedir);

    struct stat sb_link, sb_file;
    ASSERT_OK(lstat(linkpath, &sb_link));
    ASSERT_OK(stat(filepath, &sb_file));

    /* lstat() does not follow: different inode, and it's a symlink */
    TEST_ASSERT(S_ISLNK(sb_link.st_mode), 1, S_ISLNK(sb_link.st_mode));
    TEST_ASSERT(sb_link.st_ino != sb_file.st_ino,
                (intmax_t)sb_file.st_ino, (intmax_t)sb_link.st_ino);
}

/* Test 5: fstat() on a regular file via fd */
static void test_fstat_regular_file(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/regfile.txt", basedir);

    int fd = open(path, O_RDONLY);
    assert(fd >= 0);

    struct stat sb;
    ASSERT_OK(fstat(fd, &sb));

    TEST_ASSERT(S_ISREG(sb.st_mode), 1, S_ISREG(sb.st_mode));
    TEST_ASSERT(sb.st_size == 12, 12, (intmax_t)sb.st_size);

    close(fd);
}

/* Test 6: fstat() on a directory fd */
static void test_fstat_directory(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/subdir", basedir);

    int fd = open(path, O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);

    struct stat sb;
    ASSERT_OK(fstat(fd, &sb));

    TEST_ASSERT(S_ISDIR(sb.st_mode), 1, S_ISDIR(sb.st_mode));

    close(fd);
}

/* Test 7: stat() on a FIFO */
static void test_stat_fifo(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/test_fifo", basedir);
    struct stat sb;
    ASSERT_OK(stat(path, &sb));

    TEST_ASSERT(S_ISFIFO(sb.st_mode), 1, S_ISFIFO(sb.st_mode));
}

/* Test 8: verify st_mode permission bits */
static void test_stat_permission_bits(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/regfile.txt", basedir);
    struct stat sb;
    ASSERT_OK(stat(path, &sb));

    /* Created with 0644 */
    mode_t perms = sb.st_mode & 07777;
    TEST_ASSERT(perms == 0644, 0644, (intmax_t)perms);
}

/* Test 9: stat / lstat consistency on a regular file */
static void test_stat_lstat_same_for_regular(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/regfile.txt", basedir);

    struct stat sb_stat, sb_lstat;
    ASSERT_OK(stat(path, &sb_stat));
    ASSERT_OK(lstat(path, &sb_lstat));

    TEST_ASSERT(sb_stat.st_ino == sb_lstat.st_ino,
                (intmax_t)sb_lstat.st_ino, (intmax_t)sb_stat.st_ino);
    TEST_ASSERT(sb_stat.st_dev == sb_lstat.st_dev,
                (intmax_t)sb_lstat.st_dev, (intmax_t)sb_stat.st_dev);
}

/* ===========================================================================
 * Error path tests
 * =========================================================================== */

/* Test 10: ENOENT — file does not exist */
static void test_error_enoent(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/no_such_file_12345", basedir);
    struct stat sb;
    ASSERT_ERR(stat(path, &sb), ENOENT);
}

/* Test 11: ENOENT — empty pathname */
static void test_error_enoent_empty_path(void)
{
    struct stat sb;
    ASSERT_ERR(stat("", &sb), ENOENT);
}

/* Test 12: ENOENT — dangling symlink via stat() */
static void test_error_enoent_dangling_symlink(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/dangling_link", basedir);
    struct stat sb;
    /* stat() follows the link to a non-existent target => ENOENT */
    ASSERT_ERR(stat(path, &sb), ENOENT);
}

/* Test 13: ELOOP — circular symlink */
static void test_error_eloop(void)
{
    char path1[512], path2[512];
    snprintf(path1, sizeof(path1), "%s/loop_a", basedir);
    snprintf(path2, sizeof(path2), "%s/loop_b", basedir);

    assert(symlink(path2, path1) == 0);
    assert(symlink(path1, path2) == 0);

    struct stat sb;
    ASSERT_ERR(stat(path1, &sb), ELOOP);
}

/* Test 14: ENAMETOOLONG — pathname exceeds PATH_MAX */
static void test_error_enametoolong(void)
{
    /* PATH_MAX is typically 4096 on Linux */
    char longpath[5000];
    memset(longpath, 'A', sizeof(longpath) - 1);
    longpath[sizeof(longpath) - 1] = '\0';

    struct stat sb;
    ASSERT_ERR(stat(longpath, &sb), ENAMETOOLONG);
}

/* Test 15: EBADF — invalid file descriptor */
static void test_error_ebadf(void)
{
    struct stat sb;
    ASSERT_ERR(fstat(-1, &sb), EBADF);
}

/* Test 16: EBADF — closed file descriptor */
static void test_error_ebadf_closed_fd(void)
{
    int fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    close(fd);

    struct stat sb;
    ASSERT_ERR(fstat(fd, &sb), EBADF);
}

/* Test 17: EFAULT — bad address for statbuf */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull"
static void test_error_efault(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/regfile.txt", basedir);

    /* Use syscall() to bypass libc wrapper, ensuring the kernel handles
     * the bad address instead of crashing in userspace (e.g. musl). */
    long rc = -1;
    errno = ENOSYS;
#ifdef SYS_stat
    rc = syscall(SYS_stat, path, NULL);
#endif
    if (rc == -1 && errno == ENOSYS) {
        /* Fallback: try via newfstatat which covers stat on modern kernels */
        rc = syscall(SYS_newfstatat, AT_FDCWD, path, NULL, 0);
    }
    assert(rc == -1);
    assert(errno == EFAULT);
}
#pragma GCC diagnostic pop

/* Test 18: ENOTDIR — a path component is not a directory */
static void test_error_enotdir(void)
{
    /* Use the regular file as if it were a directory component */
    char path[512];
    snprintf(path, sizeof(path), "%s/regfile.txt/child", basedir);

    struct stat sb;
    ASSERT_ERR(stat(path, &sb), ENOTDIR);
}

/* Test 19: EACCES — search permission denied on a directory */
static void test_error_eacces(void)
{
    char privdir[512], filepath[512];
    snprintf(privdir, sizeof(privdir), "%s/privdir", basedir);
    snprintf(filepath, sizeof(filepath), "%s/privdir/secret.txt", basedir);

    assert(mkdir(privdir, 0000) == 0);

    /* Create file inside (need to temporarily grant execute permission) */
    assert(chmod(privdir, 0755) == 0);
    {
        int fd = open(filepath, O_CREAT | O_WRONLY, 0644);
        assert(fd >= 0);
        write(fd, "secret", 6);
        close(fd);
    }

    /* Remove all permissions from directory */
    assert(chmod(privdir, 0000) == 0);

    struct stat sb;
    /* stat() needs search (execute) permission on all path components */
    ASSERT_ERR(stat(filepath, &sb), EACCES);

    /* Restore permissions for cleanup */
    chmod(privdir, 0755);
}

/* Test 20: fstatat() with AT_SYMLINK_NOFOLLOW */
static void test_fstatat_symlink_nofollow(void)
{
    char linkpath[512];
    snprintf(linkpath, sizeof(linkpath), "%s/link_to_regfile", basedir);

    struct stat sb;
    ASSERT_OK(fstatat(AT_FDCWD, linkpath, &sb, AT_SYMLINK_NOFOLLOW));
    TEST_ASSERT(S_ISLNK(sb.st_mode), 1, S_ISLNK(sb.st_mode));
}

/* Test 21: fstatat() with invalid flags => EINVAL */
static void test_fstatat_einval(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/regfile.txt", basedir);

    struct stat sb;
    ASSERT_ERR(fstatat(AT_FDCWD, path, &sb, 0xFFFF), EINVAL);
}

/* ===========================================================================
 * Main
 * =========================================================================== */
int main(void)
{
    printf("=== stat() test suite ===\n\n");

    setup();

    printf("Normal path tests:\n");
    RUN_TEST(test_stat_regular_file);
    RUN_TEST(test_stat_directory);
    RUN_TEST(test_stat_symlink_follows);
    RUN_TEST(test_lstat_symlink_no_follow);
    RUN_TEST(test_fstat_regular_file);
    RUN_TEST(test_fstat_directory);
    RUN_TEST(test_stat_fifo);
    RUN_TEST(test_stat_permission_bits);
    RUN_TEST(test_stat_lstat_same_for_regular);

    printf("\nError path tests:\n");
    RUN_TEST(test_error_enoent);
    RUN_TEST(test_error_enoent_empty_path);
    RUN_TEST(test_error_enoent_dangling_symlink);
    RUN_TEST(test_error_eloop);
    RUN_TEST(test_error_enametoolong);
    RUN_TEST(test_error_ebadf);
    RUN_TEST(test_error_ebadf_closed_fd);
    RUN_TEST(test_error_efault);
    RUN_TEST(test_error_enotdir);
    RUN_TEST(test_error_eacces);

    printf("\nfstatat() tests:\n");
    RUN_TEST(test_fstatat_symlink_nofollow);
    RUN_TEST(test_fstatat_einval);

    teardown();

    printf("\n=== %d / %d tests passed ===\n", test_pass, test_count);
    return 0;
}
