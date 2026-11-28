#define _GNU_SOURCE
#include "console_log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CONSOLE_LOG_DIR "data/console-logs"

static int log_fd = -1;
static pthread_t relay;
static struct {
    int target;
    int original;
    int pipe_fds[2];
} streams[] = {
    {STDOUT_FILENO, -1, {-1, -1}},
    {STDERR_FILENO, -1, {-1, -1}},
};

static int write_all(int fd, const char *data, size_t size)
{
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n;
        size -= (size_t)n;
    }
    return 0;
}

static void *copy_console(void *unused)
{
    (void)unused;
    /* A closed terminal consumer must not stop file logging. */
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, NULL);

    struct pollfd inputs[] = {
        {.fd = streams[0].pipe_fds[0], .events = POLLIN},
        {.fd = streams[1].pipe_fds[0], .events = POLLIN},
    };
    char buffer[4096];
    while (inputs[0].fd >= 0 || inputs[1].fd >= 0) {
        if (poll(inputs, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (!inputs[i].revents) continue;
            ssize_t size = read(inputs[i].fd, buffer, sizeof(buffer));
            if (size < 0 && errno == EINTR) continue;
            if (size <= 0) {
                inputs[i].fd = -1;
                continue;
            }
            if (log_fd >= 0 && write_all(log_fd, buffer, (size_t)size) < 0) {
                const char warning[] = "[终端日志] 写入失败，本次运行停止保存日志文件。\n";
                close(log_fd);
                log_fd = -1;
                (void)write_all(streams[1].original, warning, sizeof(warning) - 1);
            }
            (void)write_all(streams[i].original, buffer, (size_t)size);
        }
    }
    return NULL;
}

static void restore_console(void)
{
    for (int i = 0; i < 2; ++i) {
        if (streams[i].original >= 0) {
            int result;
            do {
                result = dup2(streams[i].original, streams[i].target);
            } while (result < 0 && errno == EINTR);
            if (result < 0) close(streams[i].target);
        }
        if (streams[i].pipe_fds[1] >= 0) {
            close(streams[i].pipe_fds[1]);
            streams[i].pipe_fds[1] = -1;
        }
    }
}

static void close_console_log(void)
{
    for (int i = 0; i < 2; ++i) {
        if (streams[i].pipe_fds[0] >= 0) close(streams[i].pipe_fds[0]);
        if (streams[i].original >= 0) close(streams[i].original);
    }
    if (log_fd >= 0) close(log_fd);
}

static void finish_console_log(void)
{
    fflush(stdout);
    fflush(stderr);
    restore_console();
    pthread_join(relay, NULL);
    close_console_log();
}

void console_log_start(void)
{
    char path[128] = "";
    time_t now = time(NULL);
    struct tm local;
    char timestamp[24] = "unknown-time";
    if (localtime_r(&now, &local))
        strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local);

    if ((mkdir("data", 0755) < 0 && errno != EEXIST) ||
        (mkdir(CONSOLE_LOG_DIR, 0700) < 0 && errno != EEXIST))
        goto failed;

    snprintf(path, sizeof(path), CONSOLE_LOG_DIR "/gateway-%s-XXXXXX.log", timestamp);
    log_fd = mkostemps(path, 4, O_CLOEXEC);
    if (log_fd < 0) goto failed;
    for (int i = 0; i < 2; ++i) {
        streams[i].original = fcntl(streams[i].target, F_DUPFD_CLOEXEC, 3);
        if (streams[i].original < 0 || pipe2(streams[i].pipe_fds, O_CLOEXEC) < 0 ||
            dup2(streams[i].pipe_fds[1], streams[i].target) < 0)
            goto failed;
        close(streams[i].pipe_fds[1]);
        streams[i].pipe_fds[1] = -1;
    }
    int error = pthread_create(&relay, NULL, copy_console, NULL);
    if (error) {
        errno = error;
        goto failed;
    }
    if (atexit(finish_console_log) != 0) {
        finish_console_log();
        unlink(path);
        fprintf(stderr, "[终端日志] 无法注册退出处理，本次运行不保存日志文件。\n");
        return;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("[终端日志] 本次运行保存到 %s\n", path);
    return;

failed:;
    int saved_errno = errno;
    restore_console();
    if (log_fd >= 0) unlink(path);
    close_console_log();
    fprintf(stderr, "[终端日志] 无法开启保存：%s\n", strerror(saved_errno));
}
