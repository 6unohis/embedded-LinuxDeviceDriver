#ifndef _SCULL_H
#define _SCULL_H

#include<linux/ioctl.h>

#ifndef SCULL_MAJOR
#define SCULL_MAJOR 0
#endif

#ifndef SCULL_NR_DEVS
#define SCULL_NR_DEVS 4
#endif

#ifndef SCULL_P_NR_DEVS // for pipe
#define SCULL_P_NR_DEVS 4
#endif

#ifndef SCULL_P_BUFFER // pipe buffer size
#define SCULL_P_BUFFER 40
#endif

#ifndef SCULL_QUANTUM
#define SCULL_QUANTUM 4000
#endif

#ifndef SCULL_QSET
#define SCULL_QSET 1000
#endif

struct scull_qset{
    void **data;
    struct scull_qset* next;
};

struct scull_dev{
    struct scull_qset *data;
    int quantum;
    int qset;
    unsigned long size;
    struct semaphore sem;
    struct cdev cdev;
};

extern int scull_major;
extern int scull_nr_devs;
extern int scull_quantum;
extern int scull_qset;

// int scull_p_init(void);
void scull_p_cleanup(void);

#define SCULL_IOC_MAGIC 'k'
/* 여러분의 코드에서는 이와 다른 8비트 숫자를 사용하라 */

/* 인자가 없는 ioctl */
#define SCULL_IOCRESET _IO(SCULL_IOC_MAGIC, 0)

/*
 * S 는 포인터를 통한 "Set"을 의미,
 * T 는 인자 값으로 직접 "Tell",
 * G 는 포인터를 통한 "Get" (응답),
 * Q 는 리턴 값으로 "Query"(질의),
 * X 는 G 와 S 를 원자적으로 "eXchange",
 * H 는 T 와 Q 를 원자적으로 "sHift" 한다는 의미
 */
#define SCULL_IOCSQUANTUM _IOW(SCULL_IOC_MAGIC, 1, int)
#define SCULL_IOCSQSET    _IOW(SCULL_IOC_MAGIC, 2, int)
#define SCULL_IOCTQUANTUM _IO(SCULL_IOC_MAGIC, 3)
#define SCULL_IOCTQSET    _IO(SCULL_IOC_MAGIC, 4)
#define SCULL_IOCGQUANTUM _IOR(SCULL_IOC_MAGIC, 5, int)
#define SCULL_IOCGQSET    _IOR(SCULL_IOC_MAGIC, 6, int)
#define SCULL_IOCQQUANTUM _IO(SCULL_IOC_MAGIC, 7)
#define SCULL_IOCQQSET    _IO(SCULL_IOC_MAGIC, 8)
#define SCULL_IOCXQUANTUM _IOWR(SCULL_IOC_MAGIC, 9, int)
#define SCULL_IOCXQSET    _IOWR(SCULL_IOC_MAGIC, 10, int)
#define SCULL_IOCHQUANTUM _IO(SCULL_IOC_MAGIC, 11)
#define SCULL_IOCHQSET    _IO(SCULL_IOC_MAGIC, 12)

/* 최대 번호 (편의상 범위 체크용) */
#define SCULL_IOC_MAXNR 14


#endif