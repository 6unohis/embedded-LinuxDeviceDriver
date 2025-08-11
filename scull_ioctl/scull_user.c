#include<stdio.h>
#include<stdlib.h>
#include<fcntl.h>
#include<unistd.h>
#include<sys/ioctl.h>
#include "scull_user.h"

int main(){
    int fd = open("/dev/scull0", O_RDWR);
    if(fd < 0){
        perror("open");
        return 1;
    }

    int quantum = 6000;
    if(ioctl(fd, SCULL_IOCSQUANTUM, &quantum) == -1){
        perror("ioctl set quantum");
    }

    int get_q;
    if(ioctl(fd, SCULL_IOCGQUANTUM, &get_q) == -1){
        perror("ioctl get quantum");
    }else{
        printf("ioctl read quantum = %d\n", get_q);
    }

    close(fd);

    system("cat /proc/scullmem");
    return 0;
}