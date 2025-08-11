#include<linux/init.h>
#include<linux/module.h>
#include<linux/moduleparam.h>
#include<linux/fs.h>
#include<linux/cdev.h>
#include<linux/slab.h>
#include<linux/uaccess.h>
#include<linux/semaphore.h>

#define SCULL_QUANTUM 40
#define SCULL_QSET    10
#define DEVICE_NAME   "scull_"

int scull_major;
int scull_minor;
int scull_nr_devs = 1;

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

struct scull_dev *scull_device;
struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
    struct scull_qset *qs = dev->data;
    if(!qs){
        qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
        if(qs == NULL){
            printk(KERN_ERR "in scull_follow, qs : NULL\n");
            return NULL;
        }
        memset(qs, 0, sizeof(struct scull_qset));
    }

    while(n--){
        if(!qs->next){
            qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
            if(qs->next == NULL){
                printk(KERN_ERR "in scull_follow, qs->next : NULL\n");
                return NULL;
            }
            memset(qs->next, 0, sizeof(struct scull_qset));
        }

        qs = qs->next;
        continue;
    }
    return qs;
}

int scull_trim(struct scull_dev *dev)
{
    struct scull_qset *next, *dptr;
    int qset = dev->qset;
    int i;

    for(dptr = dev->data; dptr; dptr = next){
        if(dptr->data){
            for(i = 0; i < qset; i++)
                kfree(dptr->data[i]);
            kfree(dptr->data);
            dptr->data = NULL;
        }

        next = dptr->next;
        kfree(dptr);
    }

    dev->size = 0;
    dev->quantum = SCULL_QUANTUM;
    dev->qset = SCULL_QSET;
    dev->data = NULL;
    return 0;
}

int scull_open(struct inode *inode, struct file *filp)
{
    struct scull_dev *dev;
    dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev;
    if((filp->f_flags & O_ACCMODE) == O_WRONLY){
        scull_trim(dev);
    }

    return 0;
}

int scull_release(struct inode *inode, struct file *filp)
{
    return 0;
}

ssize_t scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *dptr;
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = 0;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;

    if (*f_pos >= dev->size)
        goto out;
    if (*f_pos + count > dev->size)
        count = dev->size - *f_pos;

    item = (long)*f_pos / itemsize;
    rest = (long)*f_pos % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;

    dptr = scull_follow(dev, item);
    if (!dptr || !dptr->data || !dptr->data[s_pos])
        goto out;

    if (count > quantum - q_pos)
        count = quantum - q_pos;

    if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;

out:
    up(&dev->sem);
    return retval;
}

ssize_t scull_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *dptr;
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = -ENOMEM;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;

    item = (long)*f_pos / itemsize;
    rest = (long)*f_pos % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;

    dptr = scull_follow(dev, item);
    if (!dptr)
        goto out;

    if (!dptr->data) {
        dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
        if (!dptr->data)
            goto out;
        memset(dptr->data, 0, qset * sizeof(char *));
    }

    if (!dptr->data[s_pos]) {
        dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
        if (!dptr->data[s_pos])
            goto out;
    }

    if (count > quantum - q_pos)
        count = quantum - q_pos;

    if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += count;
    retval = count;

    if (dev->size < *f_pos)
        dev->size = *f_pos;

out:
    up(&dev->sem);
    return retval;
}

static struct file_operations scull_fops = {
	.owner = THIS_MODULE,
	.open = scull_open,
	.release = scull_release,
	.read = scull_read,
	.write = scull_write,
};

static void scull_setup_cdev(struct scull_dev *dev, int index)
{
	int err, devno = MKDEV(scull_major, scull_minor + index);

	cdev_init(&dev->cdev, &scull_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_fops;

	err = cdev_add(&dev->cdev, devno, 1);
	if(err)
		printk(KERN_NOTICE "Error %d adding scull%d", err, index);
}

static int __init scull_init(void)
{
	dev_t dev = 0;

	alloc_chrdev_region(&dev, scull_minor, scull_nr_devs, DEVICE_NAME);
	scull_major = MAJOR(dev);

	scull_device = kmalloc(sizeof(struct scull_dev), GFP_KERNEL);
	if(!scull_device) {
		printk(KERN_ERR "Failed to allocate motherfucking scull_device!\n");
		unregister_chrdev_region(dev, scull_nr_devs);
		return -ENOMEM;
	}
	memset(scull_device, 0, sizeof(struct scull_dev));
	scull_device->quantum = SCULL_QUANTUM;
	scull_device->qset = SCULL_QSET;
	sema_init(&scull_device->sem, 1);

	scull_setup_cdev(scull_device, 0);
	printk(KERN_NOTICE "scull_: registered with major %d\n", scull_major);
	return 0;
}

static void __exit scull_exit(void)
{
	cdev_del(&scull_device->cdev);
	unregister_chrdev_region(MKDEV(scull_major, scull_minor), scull_nr_devs);
	scull_trim(scull_device);
	kfree(scull_device);
	printk(KERN_NOTICE "scull_: unregistered\n");
}

module_init(scull_init);
module_exit(scull_exit);

MODULE_LICENSE("Dual BSD/GPL");