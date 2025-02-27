#include <linux/inet.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include "rdma/rdma_cm.h"
#include "rdma/ib_verbs.h"

MODULE_LICENSE("GPL");

#define RDMA_CHARDEV_DEFAULT_NAME "myrdma"
#define RDMA_DEFAULT_SERVER_IPSTR "0.0.0.0"
#define RDMA_DEFAULT_CLIENT_IPSTR "0.0.0.0"
#define RDMA_DEFAULT_PORT 10021
#define MEM_SIZE 64
#define MAX_CQE 8192

#define _fmt(f) "myrdma: "  f "\n"
#define fmt(f) _fmt(f)

#define RDMA_LOG(f, ...) 							\
do { 												\
	printk(KERN_DEBUG fmt(f), ##__VA_ARGS__);		\
} while (0)

#define RDMA_ASSERT(e, ...)							\
	if (!(e)) {										\
		RDMA_LOG("[ASSERTION FAIL] %s:%d\n", __FILE__, __LINE__);	\
	}


// static int rdma_open(struct inode *inode, struct file *file);
// static int rdma_release(struct inode *inode, struct file *file);

static char* cdev_name = RDMA_CHARDEV_DEFAULT_NAME;
module_param_named(cdev_name, cdev_name, charp, 0);

// static char* rdma_server_ipstr = RDMA_DEFAULT_IPSTR;
// module_param_named(ip_addr, rdma_server_ipstr, charp, 0);

// static int rdma_server_port = RDMA_DEFAULT_PORT;
// module_param_named(ip_port, rdma_server_port, int, 0);

static char* msg = "Hello RDMA\n";
module_param_named(msg, msg, charp, 0);
