#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>

#include "scull.h"

// scull pipe 장치 구조체
struct scull_pipe{
    wait_queue_head_t inq, outq;       // 특정 이벤트를 기다리는 대기 큐 (read, write)
    char *buffer, *end;                // 장치 버퍼 포인터
    int buffersize;                    // 버퍼 크기
    char *rp, *wp;                     // read, write 포인터
    int nreaders, nwriters;            // reader, writer 수
    struct fasync_struct *async_queue; // 비동기 알람을 위한 큐, cat <-> echo 방식에서는 의미 없음
    struct semaphore sem;
    struct cdev cdev;
};

int scull_p_nr_devs = SCULL_P_NR_DEVS;
int scull_p_buffer = SCULL_P_BUFFER;
dev_t scull_p_devno;
struct scull_pipe *scull_p_devices;

/* 
* fasync
* scull_pipe 장치의 async_queue에 fcntl을 호출한 pid 등록
* 이후에 SIGIO 전달 시 async_queue에 등록되어있는 모든 프로세스에 전달
*/
int scull_p_fasync(int fd, struct file *filp, int mode)
{
    struct scull_pipe *dev = filp->private_data;
    return fasync_helper(fd, filp, mode, &dev->async_queue);
}

/* 
* open
*/
int scull_p_open(struct inode *inode, struct file *filp)
{
    struct scull_pipe *dev = container_of(inode->i_cdev, struct scull_pipe, cdev);
    filp->private_data = dev;

    /////////////////////////////////////////////////////////////////////
    // critical section                                                //
    if(down_interruptible(&dev->sem))                                  //
        return -ERESTARTSYS;                                           

    /*
    * 1. !dev->buffer
    *   : dev->buffer가 정의되지 않은 경우
    *   : dev->buffer 할당 필요 -> kmalloc
    * 2. !dev->buffer
    *   : kmalloc으로 할당했음에도 NULL인 경우
    *   : 세마포어 락 반납 및 에러 반환 
    */
    if(!dev->buffer){
        dev->buffer = kmalloc(scull_p_buffer, GFP_KERNEL);
        if(!dev->buffer){
            up(&dev->sem);
            return -ENOMEM;
        }
    }

    /*
    * scull_pipe 기본 설정
    * buffersize
    * buffer, end
    * rp, wp
    * nreaders, nwriters
    */
    dev->buffersize = scull_p_buffer;
    dev->end = dev->buffer + dev->buffersize;
    dev->rp = dev->wp = dev->buffer;

    if(filp->f_mode & FMODE_READ)
        dev->nreaders++;
    if(filp->f_mode & FMODE_WRITE)
        dev->nwriters++;

    up(&dev->sem);                                                     //
    // critical section                                                //
    /////////////////////////////////////////////////////////////////////
    
    return nonseekable_open(inode, filp);
}

/*
* release
*/
int scull_p_release(struct inode *inode, struct file *filp)
{
    struct scull_pipe *dev = filp->private_data;

    /*
    * cleanup fasync
    * 세번째 인자인 mode에 따라 다른 동작
    * 1: 비동기 등록
    * 0: 등록 해제
    */
    scull_p_fasync(-1, filp, 0); 

    /////////////////////////////////////////////////////////////////////
    // critical section                                                //
    down(&dev->sem);
    if(filp->f_mode & FMODE_READ)
        dev->nreaders--;
    if(filp->f_mode & FMODE_WRITE)
        dev->nwriters--;
    if(dev->nreaders + dev->nwriters == 0){
        kfree(dev->buffer);
        dev->buffer = NULL;
    }
    up(&dev->sem);
    // critical section                                                //
    /////////////////////////////////////////////////////////////////////

    return 0;
}

/*
* Read
*/
ssize_t scull_p_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    // 1. 현재 파일 포인터와 연결된 scull_pipe 호출
    struct scull_pipe *dev = filp->private_data;

    /////////////////////////////////////////////////////////////////////
    // critical section                                                //
    // 2. 세마포어 락
    if(down_interruptible(&dev->sem))
        return -ERESTARTSYS;

    /*
    * 3. 현재 버퍼가 비어있는지 확인
    *   ├ 비어있는 경우
    *   ├ 세마포어 반납, Non-Blocking 구조면 바로 반환
    *   ├ dev->inq에 현재 태스크를 넣고 컨디션 만족까지 대기 (rp와 wp가 다를 때까지)
    *   ├ 깨어나면 세마포어 락
    *   └ 3번 조건 다시 확인
    */
    while(dev->rp == dev->wp){
        up(&dev->sem);
        if(filp->f_flags & O_NONBLOCK)
            return -EAGAIN;

        printk(KERN_NOTICE "\"%s\" reading: Going to sleep\n", current->comm);
        if(wait_event_interruptible(dev->inq, dev->rp != dev->wp))
            return -ERESTARTSYS;
        if(down_interruptible(&dev->sem))
            return -ERESTARTSYS;
    }

    /*
    * 4. read 가능한 count 재계산
    *   ├ --rp--wp--end
    *   └ --wp--rp--end
    */
    if(dev->wp > dev->rp) 
        count = min(count, (size_t)(dev->wp - dev->rp));
    else
        count = min(count, (size_t)(dev->end - dev->rp));

    /*
    * 5. 사용자 공간으로 복사 및 예외처리
    */
    if(copy_to_user(buf, dev->rp, count)){
        up(&dev->sem);
        return -EFAULT;
    }

    /*
    * 6. 복사 이후 rp 위치 변경
    *   └ end까지 읽은 경우 rp위치를 원점으로
    */
    dev->rp += count;
    if(dev->rp == dev->end)
        dev->rp = dev->buffer;
    up(&dev->sem);
    // 7. 세마포어 반납
    // critical section                                                //
    /////////////////////////////////////////////////////////////////////

    // 8. 공간이 생겼으니 outq에 대기중인 태스크를 깨움
    wake_up_interruptible(&dev->outq);

    printk(KERN_NOTICE "\"%s\" did read %li bytes\n", current->comm, (long)count);
    return count;
}

/* 
* spacefree helper
* 1. dev->rp == dev->wp
*   : rp = wp인 경우는 버퍼가 비어있는 경우 뿐
*   : 따라서 buffersize - 1만큼 반환
* 2. 아닌 경우 남아 있는 만큼 반환 
*/
int spacefree(struct scull_pipe *dev)
{
    if(dev->rp == dev->wp)
        return dev->buffersize - 1;
    return ((dev->rp + dev->buffersize - dev->wp) % dev->buffersize) - 1;
}

/* 
* blocking getwritespace 
* 빈 공간이 생길 때까지 대기
*/
int scull_getwritespace(struct scull_pipe *dev, struct file *filp)
{
    /*
    * 빈 공간이 없는 경우
    *   ├ 세마포어 반납, Non-Blocking 구조면 바로 반환
    *   ├ dev->outq에 현재 태스크를 넣고 컨디션 만족까지 대기 (빈 공간이 생길 때까지)
    *   ├ 깨어나면 세마포어 락
    *   └ 조건 다시 확인
    */
    while(spacefree(dev) == 0){
        up(&dev->sem);

        if(filp->f_flags & O_NONBLOCK)
            return -EAGAIN;

        printk(KERN_NOTICE "\"%s\" writing: Going to sleep\n", current->comm);
        if(wait_event_interruptible(dev->outq, spacefree(dev) != 0))
            return -ERESTARTSYS;
        if(down_interruptible(&dev->sem))
            return -ERESTARTSYS;
    }

    // 빈 공간이 있는 경우 0 반환
    return 0;
}

/*
* Write
*/
ssize_t scull_p_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    // 1. 현재 파일 포인터와 연결된 scull_pipe 호출
    struct scull_pipe *dev = filp->private_data;
    int result;

    /////////////////////////////////////////////////////////////////////
    // critical section                                                //
    // 2. 세마포어 락
    if(down_interruptible(&dev->sem))
        return -ERESTARTSYS;

    /*
    * 3. 작성 가능한 공간이 있는지 확인
    * result = 0: 작성 가능한 공간 있음
    * result !=0: 작성 가능한 공간 없음 -> 반환
    */
    result = scull_getwritespace(dev, filp);
    if(result)
        return result;

    /*
    * 4. write 가능한 count 재계산
    *   ├ --rp--wp--end
    *   └ --wp--rp--end
    */
    count = min(count, (size_t)spacefree(dev));
    if(dev->wp >= dev->rp)
        count = min(count, (size_t)(dev->end - dev->wp));
    else
        count = min(count, (size_t)(dev->rp - dev->wp -1));

    printk(KERN_NOTICE "Going to accept %li bytes to %p from %p\n", (long)count, dev->wp, buf);
    /*
    * 5. 사용자 공간으로 복사 및 예외처리
    */
    if(copy_from_user(dev->wp, buf, count)){
        up(&dev->sem);
        return -EFAULT;
    }
    
    /*
    * 6. 복사 이후 wp 위치 변경
    *   └ end까지 작성한 경우 wp위치를 원점으로
    */
    dev->wp += count;
    if(dev->wp == dev->end)
        dev->wp = dev->buffer;
    up(&dev->sem);
    // 7. 세마포어 반납
    // critical section                                                //
    /////////////////////////////////////////////////////////////////////

    // 8. 데이터가 생겼으니 inq에 대기중인 태스크를 깨움
    wake_up_interruptible(&dev->inq);

    // 9. 비동기 알람을 대기 중인 프로세스가 있는 경우 SIGIO 전송
    if(dev->async_queue)
        kill_fasync(&dev->async_queue, SIGIO, POLL_IN);

    printk(KERN_NOTICE "\"%s\" did write %li bytes\n", current->comm, (long)count);
    return count;
}

/* 
* poll
* poll은 signal-driven인 SIGIO & fasync와 달리 event-driven 방식
* 즉 poll 호출 시 wait 상태로 기다리다가 fd가 준비되면 깨우는 방식
* 여러 fd를 한 번에 관리할 수 있다는 장점이 있음
* 또한 poll은 block -> wake up -> poll -> sleep 방식이기에 OS 스케줄링 친화적
* 반면 SIGIO는 시그널로 handler를 강제 실행시키는 방식
* poll, SIGIO 둘 중 하나만 사용해도 현재 코드에서는 문제가 발생하지 않음
* 그러나 보통 poll을 많이 활용함 
*/
unsigned int scull_p_poll(struct file *filp, poll_table *wait)
{
    struct scull_pipe *dev = filp->private_data;
    unsigned int mask = 0;
    down(&dev->sem);
    poll_wait(filp, &dev->inq, wait);
    poll_wait(filp, &dev->outq, wait);
    if(dev->rp != dev->wp)
        mask |= POLLIN | POLLRDNORM;
    if(spacefree(dev))
        mask |= POLLOUT | POLLWRNORM;
    up(&dev->sem);
    return mask;
}

/* file_operations */
static const struct file_operations scull_p_fops = {
    .owner = THIS_MODULE,
    .read = scull_p_read,
    .write = scull_p_write,
    .poll = scull_p_poll,
    .fasync = scull_p_fasync,
    .open = scull_p_open,
    .release = scull_p_release,
};

/*
* init
*/
static int __init scull_p_init(void)
{
    int i, result;
    // 1. char device 할당
    // 오류 반환 시 리턴
    result = alloc_chrdev_region(&scull_p_devno, 0, scull_p_nr_devs, "scullpipe");
    if(result < 0) return result;

    /*
    * 2. device의 수만큼 동적 할당
    * 만약 할당이 제대로 안 되는 경우 할당했던 char device 반납 후 에러 반환
    */
    scull_p_devices = kzalloc(scull_p_nr_devs * sizeof(struct scull_pipe), GFP_KERNEL);
    if(!scull_p_devices){
        unregister_chrdev_region(scull_p_devno, scull_p_nr_devs);
        return -ENOMEM;
    }

    /*
    * 3. device 수만큼 초기화
    *   ├ 세마포어
    *   ├ wait_queue
    *   └ cdev
    */
    for(i = 0; i < scull_p_nr_devs; i++){
        sema_init(&scull_p_devices[i].sem, 1);
        init_waitqueue_head(&scull_p_devices[i].inq);
        init_waitqueue_head(&scull_p_devices[i].outq);

        cdev_init(&scull_p_devices[i].cdev, &scull_p_fops);
        scull_p_devices[i].cdev.owner = THIS_MODULE;
        cdev_add(&scull_p_devices[i].cdev, scull_p_devno + i, 1);
    }

    printk(KERN_INFO "scullpipe: loaded major = %d\n", MAJOR(scull_p_devno));
    return 0;
}

static void __exit scull_p_exit(void)
{
    int i;
    /*
    * 1. device 수만큼 할당 해제
    *   ├ cdev 제거
    *   └ 장치 내 버퍼 초기화
    */
    for(i = 0; i < scull_p_nr_devs; i++){
        cdev_del(&scull_p_devices[i].cdev);
        kfree(scull_p_devices[i].buffer);
    }

    // 2. 장치 집합 할당 해제
    kfree(scull_p_devices);
    
    // 3. char device 해제
    unregister_chrdev_region(scull_p_devno, scull_p_nr_devs);
    printk(KERN_INFO "scullpipe: unloaded\n");
}

module_init(scull_p_init);
module_exit(scull_p_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("YOU");
MODULE_DESCRIPTION("scull pipe complete example");