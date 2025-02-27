#include "connection.h"

static dev_t devno;
static struct device *rdma_dev;
static struct cdev rdma_cdev;

static struct rdma_cm_id *cm_id;
static struct sockaddr server_addr;
static struct ib_device *ib_device;

static struct ib_pd *pd;
static struct ib_mr *mr;
static char *client_memory;
static struct ib_cq *cq;

static dma_addr_t dma_handle;

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

static ssize_t client_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    struct ib_sge sge = {
		.addr = (uintptr_t)dma_handle,
		.length = MEM_SIZE,
		.lkey = mr->lkey
	};
	struct ib_rdma_wr rdma_wr = {
		.wr = {
			.sg_list = &sge,
			.num_sge = 1,
			.opcode = IB_WR_RDMA_READ,
			.send_flags = IB_SEND_SIGNALED,
		},
		.remote_addr = peer_addr,
		.rkey = peer_rkey,
	};

	size_t available_bytes = MEM_SIZE - *ppos;

	int err = 0;

	if (*ppos >= MEM_SIZE) return 0;

	if (count > available_bytes) count = available_bytes;

	memset(client_memory, 0, MEM_SIZE);

    err = ib_post_send(cm_id->qp, &rdma_wr.wr, NULL);
	if (err) {
		RDMA_LOG("Error in ib_post_send from client_read: %d", err);
		return err;
	}

	RDMA_LOG("Data read from remote node via RDMA read");

	msleep(5);
	// ib_dma_sync_single_for_cpu(ib_device, dma_handle, MEM_SIZE, DMA_BIDIRECTIONAL);

    if (copy_to_user(buf, client_memory, count)) return -EFAULT;
	*ppos += count;
    return count;
}

static ssize_t client_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	// for simplicity, override the file regardless of ppos
	int err;
	struct ib_sge sge = {
		.addr = (uintptr_t)dma_handle,
		.length = MEM_SIZE,
		.lkey = mr->lkey
	};
    struct ib_rdma_wr rdma_wr = {
		.wr = {
			.sg_list = &sge,
			.num_sge = 1,
			.opcode = IB_WR_RDMA_WRITE,
			.send_flags = IB_SEND_SIGNALED,
		},
		.remote_addr = peer_addr,
		.rkey = peer_rkey,
	};

	if (count > MEM_SIZE) return 0;
	
	memset(client_memory, 0, MEM_SIZE);

    if (copy_from_user(client_memory, buf, count)) return -EFAULT;


    err = ib_post_send(cm_id->qp, &rdma_wr.wr, NULL);
	if (err) {
		RDMA_LOG("Error in ib_post_send from client_write: %d", err);
		return err;
	}

	RDMA_LOG("Data written to remote node via RDMA write: %s", client_memory);

    return count;
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
	struct ib_sge sge;
	struct ib_send_wr send_wr;

	switch (event->event) {
		case RDMA_CM_EVENT_ESTABLISHED:
			RDMA_LOG("RDMA_CM_EVENT_ESTABLISHED: %p", cm_id->context);
			recv_data = event->param.conn.private_data;
			if (recv_data) {
				peer_addr = recv_data->addr;
				peer_rkey = recv_data->rkey;
				RDMA_LOG("Connection established. peer_addr: %llu, peer_rkey: %d", peer_addr, peer_rkey);
			}
			
			snprintf(client_memory, MEM_SIZE, msg);

			sge.addr = (u64)dma_handle;
			sge.length = MEM_SIZE;
			sge.lkey = mr->lkey;

			memset(&send_wr, 0, sizeof(send_wr));
			send_wr.sg_list = &sge;
			send_wr.num_sge = 1;
			send_wr.opcode = IB_WR_SEND;
			send_wr.send_flags = IB_SEND_SIGNALED;

			err = ib_post_send(cm_id->qp, &send_wr, NULL);
			if (err) {
				RDMA_LOG("Failed to post send request: %d", err);
				break;
			}

			RDMA_LOG("Init data sent to remote node: %s", client_memory);

			break;

		default:
			RDMA_LOG("cm_handler: Event Type is %d", event->event);
			break;
	}

	return err;
}

int myrdma_resolve_addr(struct sockaddr *server_addr)
{
    int err;

	struct sockaddr client_addr;
	err = address_setup_ipv4(
			&client_addr,
			RDMA_DEFAULT_CLIENT_IPSTR,
			RDMA_DEFAULT_PORT
			);
	if (err) return err;

    err = rdma_resolve_addr(cm_id, &client_addr, server_addr, 2000);

	return err;
}

static void cq_event_handler(struct ib_event *event, void *context) {
    RDMA_LOG("CQ Event: %d", event->event);
}

static int __init myrdma_init(void)
{
	int err = 0;
	fops.read = client_read;
	fops.write = client_write;
	err = address_setup_ipv4(
		&server_addr,
		RDMA_DEFAULT_SERVER_IPSTR,
		RDMA_DEFAULT_PORT
		);
	if (err) { 
		RDMA_LOG("Error in address_setup_ipv4");
		return -EINVAL;
	}

	cm_id = rdma_create_id(
			&init_net,
			cm_handler,
			NULL,
			RDMA_PS_TCP,
			IB_QPT_RC
			);
	if (!cm_id) {
		RDMA_LOG("rdma_create_id failed");
		return -EFAULT;
	}

	client_memory = kzalloc(MEM_SIZE, GFP_KERNEL);
	if (!client_memory) {
		RDMA_LOG("kzalloc failed");
		goto L1;
	}

	err = myrdma_resolve_addr(&server_addr);
	if (err) {
		RDMA_LOG("resolve_addr failed: %d", err);
		goto L2;
	}

	ib_device = cm_id->device;
	pd = ib_alloc_pd(ib_device, IB_PD_UNSAFE_GLOBAL_RKEY);
	if (!pd) {
		RDMA_LOG("allocation of pd failed");
		goto L2;
	}
	RDMA_ASSERT(ib_device == pd->device);

	mr = ib_get_dma_mr(pd, IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ | IB_ACCESS_REMOTE_WRITE);
	if (!mr) {
		RDMA_LOG("ib_get_dma_mr failed");
		goto L3;
	}
	dma_handle = ib_dma_map_single(ib_device, client_memory, MEM_SIZE, DMA_BIDIRECTIONAL);
	if (ib_dma_mapping_error(ib_device, dma_handle) != 0) {
		RDMA_LOG("ib_dma_mapping_error!!!");
		goto L4;
	}

	cq = ib_alloc_cq(ib_device, cq_event_handler, MAX_CQE, 0, IB_POLL_WORKQUEUE);
	if (!cq) {
		RDMA_LOG("ib_alloc_cq failed");
		goto L4;
	}

	err = rdma_resolve_route(cm_id, 2000);
	if (err) {
		RDMA_LOG("rdma_resolve_route failed: %d", err);
		goto L5;
	}

	err = create_qp(cm_id);
	if (err) {
		RDMA_LOG("create_qp failed: %d", err);
		goto L5;
	}

	conn_data.addr = (uint64_t)dma_handle;
	conn_data.rkey = mr->rkey;
	err = rdma_connect(cm_id, &conn_param);
	if (err) {
		RDMA_LOG("rdma_connect failed: %d", err);
		goto L6;
	}

	err = alloc_chrdev_region(&devno, 0, 1, RDMA_CHARDEV_DEFAULT_NAME);
	if (err) {
		RDMA_LOG("alloc_chrdev_region failed: %d", err);
		goto L7;
	}

	// register device class -> /sys/class/{name}
	err = class_register(&rdma_class);
	if (err) {
		RDMA_LOG("class_register failed: %d", err);
		goto L8;
	}

	rdma_dev = device_create(&rdma_class, NULL, devno, NULL, RDMA_CHARDEV_DEFAULT_NAME);
	if (!rdma_dev) {
		RDMA_LOG("device_create failed");
		goto L9;
	}

	cdev_init(&rdma_cdev, &fops);
	rdma_cdev.owner = THIS_MODULE;

	err = cdev_add(&rdma_cdev, devno, 1);
	if (err) {
		RDMA_LOG("cdev_add failed: %d", err);
		goto L10;
	}

	RDMA_LOG("myrdma module loaded");

	return 0;

L10:device_destroy(&rdma_class, devno);
    rdma_dev = NULL;
L9: class_unregister(&rdma_class);
L8: unregister_chrdev_region(devno, 1);
L7: rdma_disconnect(cm_id);

L6: rdma_destroy_qp(cm_id);
L5: ib_free_cq(cq);
	cq = NULL;
L4: ib_dereg_mr(mr);
	mr = NULL;
L3: ib_dealloc_pd(pd);
	pd = NULL;
L2: kfree(client_memory);
    client_memory = NULL;
L1: rdma_destroy_id(cm_id);
    cm_id = NULL;
	RDMA_LOG("myrdma module load failed");
	if (err) return err;
	return -EINVAL;
}

static void __exit myrdma_exit(void)
{
	cdev_del(&rdma_cdev);
	device_destroy(&rdma_class, devno);
	class_unregister(&rdma_class);
	unregister_chrdev_region(devno, 1);


	rdma_disconnect(cm_id);
	rdma_destroy_qp(cm_id);
	ib_free_cq(cq);
	ib_dereg_mr(mr);
	ib_dealloc_pd(pd);
	kfree(client_memory);
	rdma_destroy_id(cm_id);

	RDMA_LOG("myrdma module unloaded");
}

module_init(myrdma_init);
module_exit(myrdma_exit);
