/*
 * test_signal_skip.c - 信号跳过标志应为 per-thread
 *
 * 对应 bug: BLOCK_NEXT_SIGNAL_CHECK 是全局 AtomicBool，多核竞态
 * 对应 PR:  https://github.com/rcore-os/tgoskits/pull/200
 *
 * 编译: riscv64-linux-musl-gcc -static -o test_signal_skip test_signal_skip.c -lpthread
 */

#include "test_framework.h"
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

static volatile int count_a = 0;
static volatile int count_b = 0;

static void handler_a(int sig) { (void)sig; count_a++; }
static void handler_b(int sig) { (void)sig; count_b++; }

static void *thread_a(void *arg) {
    (void)arg;
    struct sigaction sa = { .sa_handler = handler_a };
    sigaction(SIGUSR1, &sa, NULL);
    for (int i = 0; i < 100; i++) {
        raise(SIGUSR1);
        usleep(100);
    }
    return NULL;
}

static void *thread_b(void *arg) {
    (void)arg;
    struct sigaction sa = { .sa_handler = handler_b };
    sigaction(SIGUSR2, &sa, NULL);
    for (int i = 0; i < 100; i++) {
        raise(SIGUSR2);
        usleep(100);
    }
    return NULL;
}

int main(void) {
    TEST_START("signal: per-thread skip flag (no cross-thread leak)");

    pthread_t ta, tb;
    pthread_create(&ta, NULL, thread_a, NULL);
    pthread_create(&tb, NULL, thread_b, NULL);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    /*
     * 如果 skip 标志是全局的，一个线程的 rt_sigreturn 设置的标志
     * 可能被另一个线程消费，导致信号丢失。
     * 允许 5% 容差（调度抖动），但不应有大量丢失。
     */
    CHECK(count_a >= 95, "thread A received >= 95/100 SIGUSR1");
    CHECK(count_b >= 95, "thread B received >= 95/100 SIGUSR2");

    TEST_DONE();
}
