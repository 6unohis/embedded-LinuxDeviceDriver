#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

int fd;

void sigio_handler(int sig)
{
    char buf[128];
    int n = read(fd, buf, sizeof(buf)-1);
    if (n > 0) {
        buf[n] = '\0';
        printf("SIGIO: read %d bytes: %s\n", n, buf);
    }
}

int main()
{
    fd = open("/dev/scullpipe0", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open");
        exit(1);
    }

    /* SIGIO 받을 owner 등록 */
    if (fcntl(fd, F_SETOWN, getpid()) < 0) {
        perror("fcntl F_SETOWN");
        exit(1);
    }

    /* 파일을 O_ASYNC 모드로 바꿔 SIGIO 받게 */
    if (fcntl(fd, F_SETFL, O_ASYNC | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL");
        exit(1);
    }

    /* SIGIO 핸들러 등록 */
    signal(SIGIO, sigio_handler);

    printf("Waiting for SIGIO on /dev/scullpipe0 ...\n");
    while (1) {
        pause(); // 신호 기다림
    }
}
