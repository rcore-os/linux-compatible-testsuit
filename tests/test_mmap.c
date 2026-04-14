/*
 * test_mmap.c - Comprehensive test suite for mmap(2) / munmap(2)
 *
 * Based on man 2 mmap semantics. Tests cover:
 *   - Normal paths: anonymous mapping, file-backed mapping, MAP_FIXED,
 *     PROT_NONE, partial munmap, multi-page mapping
 *   - Error paths: EBADF, EINVAL (zero length / no map type / unaligned
 *     offset), EACCES (read-only fd / write-only fd), ENODEV (directory),
 *     EEXIST (MAP_FIXED_NOREPLACE overlap), ENOMEM (bad addr),
 *     EINVAL (munmap unaligned)
 *
 * Compile: gcc -o test_mmap test_mmap.c
 * Run:     ./test_mmap
 */

#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

/* ── Helpers ────────────────────────────────────────────────────── */

static long page_size;

#define RUN_TEST(fn) do {                                   \
    printf("  %-45s", #fn);                                 \
    fflush(stdout);                                         \
    test_##fn();                                            \
    printf("PASSED\n");                                     \
} while (0)

/* Temporary file helpers - each test uses a unique name to avoid clashes */
#define TMP_BASE  "/tmp/mmap_test_"

static int create_tmpfile_rw(const char *name, const void *data, size_t len)
{
    int fd = open(name, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (write(fd, data, len) != (ssize_t)len) { close(fd); return -1; }
    return fd;
}

static void expect_child_sigsegv_on_write(char *addr)
{
    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        addr[0] = 'X';
        _exit(0);
    }

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGSEGV);
}

/* ══════════════════════════════════════════════════════════════════
 * Normal path tests
 * ══════════════════════════════════════════════════════════════════ */

/*
 * Anonymous MAP_PRIVATE mapping: initial zero-fill, write then read-back,
 * then munmap.
 */
static void test_anonymous_private_rw(void)
{
    void *addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(addr != MAP_FAILED);

    /* Contents must be zero-initialized */
    assert(((char *)addr)[0] == 0);
    assert(((char *)addr)[page_size - 1] == 0);

    /* Write and verify */
    ((char *)addr)[0] = 'A';
    ((char *)addr)[page_size - 1] = 'Z';
    assert(((char *)addr)[0] == 'A');
    assert(((char *)addr)[page_size - 1] == 'Z');

    assert(munmap(addr, page_size) == 0);
}

/*
 * Anonymous MAP_SHARED mapping: write pattern, verify, unmap.
 */
static void test_anonymous_shared_rw(void)
{
    void *addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    assert(addr != MAP_FAILED);

    memset(addr, 0xAB, page_size);
    assert(((unsigned char *)addr)[0] == 0xAB);
    assert(((unsigned char *)addr)[page_size - 1] == 0xAB);

    assert(munmap(addr, page_size) == 0);
}

/*
 * File-backed MAP_PRIVATE read-only mapping: verify file content through
 * the mapping.
 */
static void test_file_private_read(void)
{
    const char *path = TMP_BASE "fpr";
    const char *data = "Hello mmap!";
    size_t dlen = strlen(data);

    int fd = create_tmpfile_rw(path, data, dlen);
    assert(fd >= 0);

    void *addr = mmap(NULL, dlen, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(addr != MAP_FAILED);
    assert(memcmp(addr, data, dlen) == 0);

    assert(munmap(addr, dlen) == 0);
    close(fd);
    unlink(path);
}

/*
 * File-backed MAP_SHARED read-write mapping: modify via mapping and
 * verify the change is visible.
 */
static void test_file_shared_rw(void)
{
    const char *path = TMP_BASE "fsr";
    const char *orig = "original";
    size_t dlen = strlen(orig);

    int fd = create_tmpfile_rw(path, orig, dlen);
    assert(fd >= 0);

    void *addr = mmap(NULL, dlen, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    assert(addr != MAP_FAILED);

    memcpy(addr, "modified", dlen);
    assert(memcmp(addr, "modified", dlen) == 0);

    assert(munmap(addr, dlen) == 0);
    close(fd);
    unlink(path);
}

/*
 * File-backed mapping with non-zero page-aligned offset.
 */
static void test_file_mapping_with_offset(void)
{
    const char *path = TMP_BASE "off";
    /* Create a file of 3 pages */
    size_t flen = page_size * 3;
    int fd = create_tmpfile_rw(path, "AA", 2);
    assert(fd >= 0);
    assert(ftruncate(fd, flen) == 0);

    /* Map starting at page_size offset */
    void *addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, page_size);
    assert(addr != MAP_FAILED);

    ((char *)addr)[0] = 'B';
    assert(((char *)addr)[0] == 'B');

    assert(munmap(addr, page_size) == 0);
    close(fd);
    unlink(path);
}

/*
 * Multi-page (4 pages) anonymous mapping: touch every page.
 */
static void test_multi_page_mapping(void)
{
    size_t len = page_size * 4;
    void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(addr != MAP_FAILED);

    for (size_t i = 0; i < 4; i++)
        ((char *)addr)[i * page_size] = (char)('0' + i);

    for (size_t i = 0; i < 4; i++)
        assert(((char *)addr)[i * page_size] == (char)('0' + i));

    assert(munmap(addr, len) == 0);
}

/*
 * MAP_FIXED: allocate a region, then remap it at the same address with
 * MAP_FIXED.  Per man page this is the only safe use of MAP_FIXED.
 */
static void test_map_fixed(void)
{
    size_t len = page_size * 2;
    void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(addr != MAP_FAILED);

    void *addr2 = mmap(addr, len, PROT_READ | PROT_WRITE,
                       MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    assert(addr2 == addr);

    ((char *)addr)[0] = 'F';
    assert(((char *)addr)[0] == 'F');

    assert(munmap(addr, len) == 0);
}

/*
 * PROT_NONE: mapping exists but any access causes SIGSEGV.
 * We can only safely verify the mapping succeeds and munmap works.
 */
static void test_prot_none(void)
{
    void *addr = mmap(NULL, page_size, PROT_NONE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(addr != MAP_FAILED);
    assert(munmap(addr, page_size) == 0);
}

/*
 * mprotect(PROT_READ) should forbid writes; restoring PROT_READ|PROT_WRITE
 * should allow writes again.
 */
static void test_mprotect_readonly_then_restore(void)
{
    char *addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(addr != MAP_FAILED);

    addr[0] = 'A';
    assert(mprotect(addr, page_size, PROT_READ) == 0);
    assert(addr[0] == 'A');
    expect_child_sigsegv_on_write(addr);

    assert(mprotect(addr, page_size, PROT_READ | PROT_WRITE) == 0);
    addr[0] = 'B';
    assert(addr[0] == 'B');

    assert(munmap(addr, page_size) == 0);
}

/*
 * munmap of an already-unmapped region: the man page states "It is not an
 * error if the indicated range does not contain any mapped pages."
 */
static void test_munmap_double(void)
{
    void *addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(addr != MAP_FAILED);
    assert(munmap(addr, page_size) == 0);
    /* Second unmap on the same range should also succeed */
    assert(munmap(addr, page_size) == 0);
}

/*
 * Partial munmap: unmap the middle of a 4-page mapping, splitting it
 * into two separate mappings.  Verify first and last pages remain.
 */
static void test_partial_munmap(void)
{
    size_t len = page_size * 4;
    void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(addr != MAP_FAILED);

    /* Unmap the middle two pages */
    assert(munmap((char *)addr + page_size, page_size * 2) == 0);

    /* First and last pages still usable */
    ((char *)addr)[0] = 'X';
    assert(((char *)addr)[0] == 'X');
    ((char *)addr)[page_size * 3] = 'Y';
    assert(((char *)addr)[page_size * 3] == 'Y');

    /* Clean up remaining pages */
    assert(munmap(addr, page_size) == 0);
    assert(munmap((char *)addr + page_size * 3, page_size) == 0);
}

/*
 * Closing fd after mmap does NOT invalidate the mapping (per man page).
 */
static void test_fd_close_no_invalidate(void)
{
    const char *path = TMP_BASE "fdc";
    const char *data = "still here";
    size_t dlen = strlen(data);

    int fd = create_tmpfile_rw(path, data, dlen);
    assert(fd >= 0);

    void *addr = mmap(NULL, dlen, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(addr != MAP_FAILED);

    close(fd);  /* Close fd — mapping must still be valid */

    assert(memcmp(addr, data, dlen) == 0);
    assert(munmap(addr, dlen) == 0);
    unlink(path);
}

/* ══════════════════════════════════════════════════════════════════
 * Error path tests
 * ══════════════════════════════════════════════════════════════════ */

/*
 * EBADF: fd is not valid and MAP_ANONYMOUS is NOT set.
 */
static void test_ebadf(void)
{
    void *addr = mmap(NULL, page_size, PROT_READ,
                      MAP_PRIVATE, -1 /* invalid fd */, 0);
    assert(addr == MAP_FAILED);
    assert(errno == EBADF);
}

/*
 * EINVAL: length is 0 (since Linux 2.6.12).
 */
static void test_einval_zero_length(void)
{
    void *addr = mmap(NULL, 0, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(addr == MAP_FAILED);
    assert(errno == EINVAL);
}

/*
 * EINVAL: flags contains none of MAP_PRIVATE, MAP_SHARED, or
 * MAP_SHARED_VALIDATE.  MAP_ANONYMOUS alone does not specify a
 * mapping type.
 */
static void test_einval_no_map_type(void)
{
    void *addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS /* no PRIVATE/SHARED */, -1, 0);
    assert(addr == MAP_FAILED);
    assert(errno == EINVAL);
}

/*
 * EINVAL: offset is not a multiple of the page size.
 */
static void test_einval_unaligned_offset(void)
{
    const char *path = TMP_BASE "uoff";
    int fd = create_tmpfile_rw(path, "data_for_offset_test", 21);
    assert(fd >= 0);

    void *addr = mmap(NULL, page_size, PROT_READ,
                      MAP_PRIVATE, fd, 1 /* not page-aligned */);
    assert(addr == MAP_FAILED);
    assert(errno == EINVAL);

    close(fd);
    unlink(path);
}

/*
 * EACCES: MAP_SHARED + PROT_WRITE requested, but fd is open O_RDONLY.
 * Per man page: "MAP_SHARED was requested and PROT_WRITE is set, but fd
 * is not open in read/write (O_RDWR) mode."
 */
static void test_eacces_shared_write_ro_fd(void)
{
    const char *path = TMP_BASE "ero";
    int fd = create_tmpfile_rw(path, "test", 4);
    assert(fd >= 0);
    close(fd);

    /* Reopen read-only */
    fd = open(path, O_RDONLY);
    assert(fd >= 0);

    void *addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    assert(addr == MAP_FAILED);
    assert(errno == EACCES);

    close(fd);
    unlink(path);
}

/*
 * EACCES: fd opened O_WRONLY, mapping requests PROT_READ.
 * Per man page: "a file mapping was requested, but fd is not open for
 * reading."
 */
static void test_eacces_writeonly_fd(void)
{
    const char *path = TMP_BASE "ewo";
    int fd = create_tmpfile_rw(path, "test", 4);
    assert(fd >= 0);
    close(fd);

    fd = open(path, O_WRONLY);
    assert(fd >= 0);

    void *addr = mmap(NULL, page_size, PROT_READ,
                      MAP_PRIVATE, fd, 0);
    assert(addr == MAP_FAILED);
    assert(errno == EACCES);

    close(fd);
    unlink(path);
}

/*
 * ENODEV: mapping a directory (non-regular file).
 * Per man page: "The underlying filesystem of the specified file does not
 * support memory mapping."
 */
static void test_enODEV_directory(void)
{
    const char *dir = TMP_BASE "dir";
    (void)mkdir(dir, 0755);

    int fd = open(dir, O_RDONLY);
    if (fd < 0) {
        /* Some systems restrict opening directories; skip if so */
        rmdir(dir);
        printf("SKIPPED (cannot open dir) ");
        return;
    }

    void *addr = mmap(NULL, page_size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(addr == MAP_FAILED);
    assert(errno == ENODEV);

    close(fd);
    rmdir(dir);
}

/*
 * EEXIST: MAP_FIXED_NOREPLACE was specified and the range collides with
 * an existing mapping.  (Requires Linux >= 4.17)
 */
static void test_eexist_fixed_noreplace(void)
{
    void *addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(addr != MAP_FAILED);

    void *addr2 = mmap(addr, page_size, PROT_READ | PROT_WRITE,
                       MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE,
                       -1, 0);
    assert(addr2 == MAP_FAILED);
    assert(errno == EEXIST);

    assert(munmap(addr, page_size) == 0);
}

/*
 * ENOMEM: MAP_FIXED with an address beyond the process virtual address
 * space.
 * Per man page: "We don't like addr, because it exceeds the virtual
 * address space of the CPU."
 */
static void test_enomem_exceeds_vaddr(void)
{
    void *bad_addr = (void *)(1UL << 50);
    void *addr = mmap(bad_addr, page_size, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    assert(addr == MAP_FAILED);
    assert(errno == ENOMEM);
}

/*
 * munmap EINVAL: addr is not a multiple of the page size.
 */
static void test_munmap_einval_unaligned(void)
{
    void *addr = mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(addr != MAP_FAILED);

    /* Non-page-aligned address */
    int rc = munmap((void *)((char *)addr + 1), page_size);
    assert(rc == -1);
    assert(errno == EINVAL);

    assert(munmap(addr, page_size * 2) == 0);
}

/*
 * EACCES: a read-only shared file mapping cannot be upgraded to PROT_WRITE.
 */
static void test_mprotect_eacces_readonly_file(void)
{
    const char *path = TMP_BASE "mprot_ro";
    int fd = create_tmpfile_rw(path, "abcd", 4);
    assert(fd >= 0);
    close(fd);
    assert(chmod(path, 0444) == 0);

    fd = open(path, O_RDONLY);
    assert(fd >= 0);

    void *addr = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd, 0);
    assert(addr != MAP_FAILED);

    errno = 0;
    assert(mprotect(addr, page_size, PROT_READ | PROT_WRITE) == -1);
    assert(errno == EACCES);

    assert(munmap(addr, page_size) == 0);
    close(fd);
    unlink(path);
}

/*
 * EINVAL: addr must be page aligned.
 */
static void test_mprotect_einval_unaligned_addr(void)
{
    char *addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(addr != MAP_FAILED);

    errno = 0;
    assert(mprotect(addr + 1, page_size, PROT_READ) == -1);
    assert(errno == EINVAL);

    assert(munmap(addr, page_size) == 0);
}

/*
 * ENOMEM: the target range is no longer mapped.
 */
static void test_mprotect_enomem_unmapped(void)
{
    void *addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(addr != MAP_FAILED);
    assert(munmap(addr, page_size) == 0);

    errno = 0;
    assert(mprotect(addr, page_size, PROT_READ) == -1);
    assert(errno == ENOMEM);
}

/* ══════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════ */

int main(void)
{
    page_size = sysconf(_SC_PAGE_SIZE);
    assert(page_size > 0);
    printf("Page size: %ld bytes\n\n", page_size);

    printf("=== Normal path tests ===\n");
    RUN_TEST(anonymous_private_rw);
    RUN_TEST(anonymous_shared_rw);
    RUN_TEST(file_private_read);
    RUN_TEST(file_shared_rw);
    RUN_TEST(file_mapping_with_offset);
    RUN_TEST(multi_page_mapping);
    RUN_TEST(map_fixed);
    RUN_TEST(prot_none);
    RUN_TEST(mprotect_readonly_then_restore);
    RUN_TEST(munmap_double);
    RUN_TEST(partial_munmap);
    RUN_TEST(fd_close_no_invalidate);

    printf("\n=== Error path tests ===\n");
    RUN_TEST(ebadf);
    RUN_TEST(einval_zero_length);
    RUN_TEST(einval_no_map_type);
    RUN_TEST(einval_unaligned_offset);
    RUN_TEST(eacces_shared_write_ro_fd);
    RUN_TEST(eacces_writeonly_fd);
    RUN_TEST(enODEV_directory);
    RUN_TEST(eexist_fixed_noreplace);
    RUN_TEST(enomem_exceeds_vaddr);
    RUN_TEST(munmap_einval_unaligned);
    RUN_TEST(mprotect_eacces_readonly_file);
    RUN_TEST(mprotect_einval_unaligned_addr);
    RUN_TEST(mprotect_enomem_unmapped);

    printf("\nAll tests passed!\n");
    return 0;
}
