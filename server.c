#include "connection.h"

static dev_t devno;
static struct device *rdma_dev;
static struct cdev rdma_cdev;

static struct rdma_cm_id *cm_id;
static struct sockaddr server_addr;
static struct ib_device *ib_device;

static struct ib_pd *pd;
static struct ib_mr *mr;
static char *server_memory;
static char *remote_dma_addr;
static struct ib_cq *cq;
static struct ib_srq *srq;

static dma_addr_t dma_handle;

static struct conn_ctx {
	struct completion done;
	int ret;
} conn_context;

static struct conn_data {
	uint64_t addr;
	uint32_t rkey;
} conn_data;

static uint32_t peer_rkey;
static uint64_t peer_addr;

static struct rdma_conn_param conn_param = {
	.private_data =  &conn_data,
	.private_data_len = sizeof(struct conn_data),
	.responder_resources = 16,
	.initiator_depth = 16,
	.retry_count = 7,
	.rnr_retry_count = 7,
};

static char* devnode(struct device* dev, umode_t* mode)
{
	if (mode) *mode = 0666;
	return NULL;
}

static struct class rdma_class  = {
	.name = "rdma",
	.owner = THIS_MODULE,
	.devnode = devnode,
};

struct file_operations fops = {
	.owner = THIS_MODULE,
	.read = NULL,
	.write = NULL,
};

static ssize_t server_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	memset(server_memory, 0, MEM_SIZE);
	if (copy_from_user(server_memory, buf, count)) return -EFAULT;
	return count;
}

static ssize_t server_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{

	size_t available_bytes = MEM_SIZE - *ppos;

	if (*ppos >= MEM_SIZE) return 0;

	if (count > available_bytes) count = available_bytes;

	if (copy_to_user(buf, server_memory, count)) return -EFAULT;

	*ppos += count;

	return count;
}

static void mock_event_handler(struct ib_event *event, void *ctx)
{
	RDMA_LOG("mock_event_handler: Event Type is %d", event->event);
}

static int create_qp(struct rdma_cm_id *cm_id)
{
	int err;

	struct ib_qp_init_attr qp_init_attr = {
		.event_handler = mock_event_handler,
		.qp_context = NULL,
		.send_cq = cq,
		.recv_cq = cq,
		.cap = {
			.max_send_wr = 10,
			.max_recv_wr = 10,
			.max_send_sge = 1,
			.max_recv_sge = 1,
			.max_rdma_ctxs = 1,
			.max_inline_data = 0,
		},
		.sq_sig_type = IB_SIGNAL_ALL_WR,
		.qp_type = IB_QPT_RC,
	};

	err = rdma_create_qp(cm_id, pd, &qp_init_attr);
	return err;
}

static int cm_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *event)
{
	int err = 0;
	const struct conn_data *recv_data;

	switch (event->event) {
		case RDMA_CM_EVENT_CONNECT_REQUEST:
      		RDMA_LOG("RDMA_CM_EVENT_CONNECT_REQUEST");
			err = create_qp(cm_id);
			if (err) {
				RDMA_LOG("create_qp failed: %d", err);
				break;
			}

			conn_data.rkey = mr->rkey;
			conn_data.addr = (uint64_t)dma_handle;

			// qp_state seems to automatically go into RTS state after rdma_accept?
			err = rdma_accept(cm_id, &conn_param);
			if (err) {
				RDMA_LOG("rdma_accept failed: %d", err);
				break;
			}
			break;

		case RDMA_CM_EVENT_ESTABLISHED:
			RDMA_LOG("RDMA_CM_EVENT_ESTABLISHED: %p", cm_id->context);
			struct ib_sge recv_sge;
			struct ib_recv_wr recv_wr;
			struct ib_recv_wr *bad_recv_wr;

			struct ib_wc wc;

			recv_sge.addr = (u64)dma_handle;
			recv_sge.length = MEM_SIZE;
			recv_sge.lkey = mr->lkey;

			memset(&recv_wr, 0, sizeof(recv_wr));
			recv_wr.sg_list = &recv_sge;
			recv_wr.num_sge = 1;

			err = ib_post_recv(cm_id->qp, &recv_wr, NULL);
			if (err) {
				RDMA_LOG("Failed to post recv request: %d", err);
				break;
			}
			
			break;

		default:
			RDMA_LOG("cm_handler: Event Type is %d", event->event);
			break;
	}

	return err;
}

static struct rdma_cm_id* rdma_server_setup(struct sockaddr *addr)
{
	int err;
	struct rdma_cm_id *cm_id = rdma_create_id(
			&init_net,
			cm_handler,
			NULL,
			RDMA_PS_TCP,
			IB_QPT_RC
			);
	if (!cm_id) return NULL;

    // set RDMA socket to be reusable
	err = rdma_set_reuseaddr(cm_id, 1);
	if (err) goto FAIL;

    // bind RDMA socket to the IP/port
	err = rdma_bind_addr(cm_id, addr);
	if (err) goto FAIL;

	ib_device = cm_id->device;

	err = rdma_listen(cm_id, 5);
	if (err) goto FAIL;

	return cm_id;

FAIL:
	rdma_destroy_id(cm_id);
	return NULL;
}

static int address_setup_ipv4(struct sockaddr *addr, const char *ipv4, __be16 port)
{
	int err;
	struct sockaddr_in *saddr = (struct sockaddr_in*)addr;
	saddr->sin_family = AF_INET;
	saddr->sin_port = htons(port);

    // IPv4 textual presentation to network; returns 1 on success, zero on failure
	err = in4_pton(ipv4, -1, (u8*)&saddr->sin_addr, -1, NULL);
	if (!err) return -EINVAL;

	return 0;
}

int rdma_launch_server(const char *ipstr, uint16_t port)
{
	int err;
	err = address_setup_ipv4(
			&server_addr,
			ipstr,
			port
			);
	if (err) {
		RDMA_LOG("address_setup_ipv4 failed: %d", err);
		return err;
	}

	cm_id = rdma_server_setup(&server_addr);
	if (!cm_id) {
		RDMA_LOG("rdma_server_setup failed");
	  	return -EINVAL;
  	}

	pd = ib_alloc_pd(ib_device, IB_PD_UNSAFE_GLOBAL_RKEY);
	if (!pd) {
		RDMA_LOG("ib_alloc_pd failed");
		goto fail;
	}
	RDMA_ASSERT(ib_device == pd->device);

	mr = ib_get_dma_mr(pd, IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ | IB_ACCESS_REMOTE_WRITE);
	if (!mr) {
		RDMA_LOG("ib_get_dma_mr failed");
		goto PD;
	}
	dma_handle = ib_dma_map_single(ib_device, server_memory, MEM_SIZE, DMA_BIDIRECTIONAL);
	if (ib_dma_mapping_error(ib_device, dma_handle) != 0) {
        RDMA_LOG("ib_dma_mapping_error!!!");
		goto MR;
	} 
	RDMA_LOG("TCP server for RDMA connection launched (ib_addr: %s, ib_port: %d)", ipstr, port);

	return 0;
MR:
    ib_dereg_mr(mr);
	mr = NULL;
PD:
    ib_dealloc_pd(pd);
	pd = NULL;
fail:
    return -EINVAL;
}

void rdma_shutdown_server(void)
{
	pd = NULL;

	if (cm_id) rdma_destroy_id(cm_id);
	cm_id = NULL;

	RDMA_LOG("TCP server for RDMA has been shutdown");
}

static int setup_rdma_memory(void)
{
	server_memory = kzalloc(MEM_SIZE, GFP_KERNEL);
	if (!server_memory) return -ENOMEM;

	return 0;
}

static void cq_event_handler(struct ib_event *event, void *context) {
    RDMA_LOG("CQ Event: %d", event->event);
}

static int __init myrdma_init(void)
{
	int err = 0;

	fops.read = server_read;
	fops.write = server_write;
	err = setup_rdma_memory();
	if (err) {
		RDMA_LOG("memory setup failed");
		goto fail;
	}

	err = rdma_launch_server(RDMA_DEFAULT_SERVER_IPSTR, RDMA_DEFAULT_PORT);
	if (err) goto fail0;

	cq = ib_alloc_cq(ib_device, cq_event_handler, MAX_CQE, 0, IB_POLL_WORKQUEUE);
	if (!cq) {
		RDMA_LOG("ib_alloc_cq failed");
		goto fail0;
	}

	err = alloc_chrdev_region(&devno, 0, 1, RDMA_CHARDEV_DEFAULT_NAME);
	if (err) {
		RDMA_LOG("alloc_chrdev_region failed: %d", err);
		goto L1;
	}

	err = class_register(&rdma_class);
	if (err) {
		RDMA_LOG("class_register failed: %d", err);
		goto L2;
	}

	rdma_dev = device_create(&rdma_class, NULL, devno, NULL, RDMA_CHARDEV_DEFAULT_NAME);
	if (!rdma_dev) {
		RDMA_LOG("device_create failed");
		goto L3;
	}

	cdev_init(&rdma_cdev, &fops);
	rdma_cdev.owner = THIS_MODULE;

	err = cdev_add(&rdma_cdev, devno, 1);
	if (err) {
		RDMA_LOG("cdev_add failed: %d", err);
		goto L4;
	}

	RDMA_LOG("myrdma module loaded");

	return 0;

L4: device_destroy(&rdma_class, devno);
    rdma_dev = NULL;
L3: class_unregister(&rdma_class);
L2: unregister_chrdev_region(devno, 1);
L1: ib_free_cq(cq);
	cq = NULL;

fail0:
	kfree(server_memory);

fail:
	RDMA_LOG("myrdma module load failed");
	return err;
}

static void __exit myrdma_exit(void)
{
	cdev_del(&rdma_cdev);
	device_destroy(&rdma_class, devno);
	class_unregister(&rdma_class);
	unregister_chrdev_region(devno, 1);

	rdma_shutdown_server();
	kfree(server_memory);


	RDMA_LOG("myrdma module unloaded");
}

module_init(myrdma_init);
module_exit(myrdma_exit);
