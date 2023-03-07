/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright 2019-2022 NXP
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#include "ipc-shm.h"
#include "ipcf_Ip_Cfg.h"

#define IPC_SHM_DEV_MEM_NAME    "/dev/mem"

#ifdef POLLING
#define INTER_CORE_TX_IRQ IPC_IRQ_NONE
#else
#define INTER_CORE_TX_IRQ 2u
#endif /* POLLING */
#define INTER_CORE_RX_IRQ 1u

#define CTRL_CHAN_ID 0
#define CTRL_CHAN_SIZE 64
#define MAX_SAMPLE_MSG_LEN 32
#define L_BUF_LEN 4096
#define IPC_SHM_SIZE 0x100000
#define LOCAL_SHM_ADDR 0x34100000

/* convenience wrappers for printing messages */
#define pr_fmt(fmt) "ipc-shm-us-app: %s(): "fmt
#define sample_err(fmt, ...) printf(pr_fmt(fmt), __func__, ##__VA_ARGS__)
#define sample_info(fmt, ...) printf("ipc-shm-us-app: "fmt, ##__VA_ARGS__)
#ifdef DEBUG
#define sample_dbg(fmt, ...) printf(pr_fmt(fmt), __func__, ##__VA_ARGS__)
#else
#define sample_dbg(fmt, ...)
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

static int msg_sizes[IPC_SHM_MAX_POOLS] = {MAX_SAMPLE_MSG_LEN};
//static int msg_sizes[IPC_SHM_MAX_POOLS] = {3*1024};
static int msg_sizes_count = 1;

/**
 * struct ipc_sample_app - sample app private data
 * @num_channels:		number of channels configured in this sample
 * @num_msgs:			number of messages to be sent to remote app
 * @ctrl_shm:			control channel local shared memory
 * @last_rx_no_msg:		last number of received message
 * @last_tx_msg:		last transmitted message
 * @last_rx_msg:		last received message
 * @local_virt_shm:		local shared memory virtual address
 * @mem_fd:				MEM device file descriptor
 * @local_shm_offset:	local ShM offset in mapped page
 * @local_shm_map:		local ShM mapped page address
 * @sema:				binary semaphore for sync send_msg func with shm_rx_cb
 * @instance:			instance id
 */
static struct ipc_sample_app {
	int num_channels;
	int num_msgs;
	char *ctrl_shm;
	int last_rx_no_msg;
	char last_tx_msg[L_BUF_LEN];
	char last_rx_msg[L_BUF_LEN];
	int *local_virt_shm;
	int mem_fd;
	size_t local_shm_offset;
	int *local_shm_map;
	sem_t sema;
	uint8_t instance;
} app;

/* link with generated variables */
const void *rx_cb_arg = &app;

/* Init IPC shared memory driver (see ipc-shm.h for API) */
static int init_ipc_shm(void)
{
	int err;

	err = ipc_shm_init(&ipcf_shm_instances_cfg);
	if (err)
		return err;

	app.num_channels = ipcf_shm_instances_cfg.shm_cfg[0].num_channels;

	/* acquire control channel memory once */
	// app.ctrl_shm = ipc_shm_unmanaged_acquire(app.instance, CTRL_CHAN_ID);
	// if (!app.ctrl_shm) {
	// 	sample_err("failed to get memory of control channel");
	// 	return -ENOMEM;
	// }

	return 0;
}

/*
 * data channel Rx callback: print message, release buffer and signal the
 * completion variable.
 */
void data_chan_rx_cb(void *arg, const uint8_t instance, int chan_id,
		void *buf, size_t size)
{
	int err = 0;
	char *endptr;

	assert(size <= L_BUF_LEN);

	/* process the received data */
	memcpy(app.last_rx_msg, buf, size);
	sample_info("ch %d << %ld bytes: %s\n", chan_id, size, app.last_rx_msg);

	/* consume received data: get number of message */
	/* Note: without being copied locally */
	app.last_rx_no_msg = strtol((char *)buf + strlen("#"), &endptr, 10);

	/* release the buffer */
	err = ipc_shm_release_buf(instance, chan_id, buf);
	if (err) {
		sample_err("failed to free buffer for channel %d,"
			    "err code %d\n", chan_id, err);
	}

	/* notify send_msg function a reply was received */
	sem_post(&app.sema);
}

/*
 * control channel Rx callback: print control message
 */
void ctrl_chan_rx_cb(void *arg, const uint8_t instance, int chan_id,
		void *mem)
{
	/* temp buffer for string operations that do unaligned SRAM accesses */
	char tmp[CTRL_CHAN_SIZE] = {0};

	assert(chan_id == CTRL_CHAN_ID);
	assert(strlen(mem) <= L_BUF_LEN);

	memcpy(tmp, mem, CTRL_CHAN_SIZE);
	sample_info("ch %d << %ld bytes: %s\n",
		    chan_id, strlen(tmp), (char *)tmp);

//	/* notify run_demo() the ctrl reply was received and demo can end */
//	sem_post(&app.sema);
}

/* send control message with number of data messages to be sent */
static int send_ctrl_msg(const uint8_t instance)
{
	/* last channel is control channel */
	const int chan_id = CTRL_CHAN_ID;
	/* temp buffer for string operations that do unaligned SRAM accesses */
	char tmp[CTRL_CHAN_SIZE] = {0};
	int err;

	/* Write number of messages to be sent in control channel memory */
	sprintf(tmp, "SENDING MESSAGES: %d", app.num_msgs);
	memcpy(app.ctrl_shm, tmp, CTRL_CHAN_SIZE);

	sample_info("ch %d >> %ld bytes: %s\n", chan_id, strlen(tmp), tmp);

	/* notify remote */
	// err = ipc_shm_unmanaged_tx(instance, chan_id);
	// if (err) {
	// 	sample_err("tx failed on control channel");
	// 	return err;
	// }

	return 0;
}

/**
 * generate_msg() - generates message with fixed pattern
 * @dest:	destination buffer
 * @len:	destination buffer length
 * @msg_no:	message number
 *
 * Generate message by repeated concatenation of a pattern
 */
static void generate_msg(char *dest, int len, int msg_no)
{
	static char *msg_pattern = "HELLO WORLD!";

	snprintf(dest, len, "#%d %s", msg_no, msg_pattern);
}

/**
 * send_data_msg() - Send generated data message to remote peer
 * @instance: instance id
 * @msg_len: message length
 * @msg_no: message sequence number to be written in the test message
 * @chan_id: ipc channel to be used for remote CPU communication
 *
 * It uses a completion variable for synchronization with reply callback.
 */
static int send_data_msg(const uint8_t instance, int msg_len, int msg_no,
		int chan_id)
{
	int err = 0;
	char *buf = NULL;

	buf = ipc_shm_acquire_buf(instance, chan_id, msg_len);
	if (!buf) {
		sample_err("failed to get buffer for channel ID"
			   " %d and size %d\n", chan_id, msg_len);
		return -ENOMEM;
	}

	/* write data to acquired buffer */
	generate_msg(buf, msg_len, msg_no);

	/* save data for comparison with echo reply */
	memcpy(app.last_tx_msg, buf, msg_len);

	sample_info("ch %d >> %d bytes: %s\n", chan_id, msg_len,
		    app.last_tx_msg);

	/* send data to remote peer */
	err = ipc_shm_tx(instance, chan_id, buf, msg_len);
	if (err) {
		sample_err("tx failed for channel ID %d, size "
			   "%d, error code %d\n", 0, msg_len, err);
		return err;
	}

	/* wait for echo reply from remote (signaled from Rx callback) */
//	sem_wait(&app.sema);
//	if (errno == EINTR) {
//		sample_info("interrupted...\n");
//		return err;
//	}

	/* check if received message match with sent message */
//	if (app.last_rx_no_msg != msg_no) {
//		sample_err("last_rx_no_msg != msg_no\n");
//		sample_err(">> #%d\n", msg_no);
//		sample_err("<< #%d\n", app.last_rx_no_msg);
//		return -EINVAL;
//	}

	return 0;
}

/*
 * Send requested number of messages to remote peer, cycling through all data
 * channels and wait for an echo reply after each sent message.
 */
static int run_demo(int num_msgs)
{
	int err, msg, ch, i;

	/* signal number of messages to remote via control channel */
//	err = send_ctrl_msg(app.instance);
//	if (err)
//		return err;

	/* send generated data messages */
	msg = 0;
	while (msg < num_msgs) {
		for (ch = 0/*CTRL_CHAN_ID + 1*/; ch < app.num_channels; ch++) {
			for (i = 0; i < msg_sizes_count; i++) {
				err = send_data_msg(app.instance,
						msg_sizes[i], msg, ch);
				if (err)
					return err;

                                /* wait for echo reply from remote (signaled from Rx callback) */
                                sem_wait(&app.sema);

				if (++msg == num_msgs) {					
					return 0;
				}
			}
		}
	}

	return 0;
}

/*
 * interrupt signal handler for terminating the sample execution gracefully
 */
void int_handler(int signum)
{
	app.num_msgs = 0;
}

int main(int argc, char *argv[])
{
	int err = 0;
	struct sigaction sig_action;
	app.instance = 0;
	size_t page_size = sysconf(_SC_PAGE_SIZE);
	off_t page_phys_addr;
	uintptr_t local_shm_addr = LOCAL_SHM_ADDR;
	uint32_t shm_size = IPC_SHM_SIZE;
	int tmp[IPC_SHM_SIZE] = {0};

	sem_init(&app.sema, 0, 0);

	/* init ipc shm driver */
	err = init_ipc_shm();
	if (err)
		return err;

	/* catch interrupt signals to terminate the execution gracefully */
	sig_action.sa_handler = int_handler;
	sigaction(SIGINT, &sig_action, NULL);

#if 0
	app.num_msgs = 1;
	while (app.num_msgs) {
		printf("\nInput number of messages to send: ");
		scanf("%d", &app.num_msgs);

		if (!app.num_msgs)
			break;

		err = run_demo(app.num_msgs);
		if (err)
			break;
	}
#else
        int test_cnt = 0;
        while (1) {
            err = run_demo(2);
            if (err)
                break;
            sleep(1);
        }
#endif

	ipc_shm_free();

	/* Clear memory to re-init driver */
	page_phys_addr = (local_shm_addr / page_size) * page_size;
	app.local_shm_offset = local_shm_addr - page_phys_addr;
	app.mem_fd = open(IPC_SHM_DEV_MEM_NAME, O_RDWR);

	app.local_shm_map = mmap(NULL, app.local_shm_offset + shm_size,
				  PROT_READ | PROT_WRITE, MAP_SHARED,
				  app.mem_fd, page_phys_addr);

	app.local_virt_shm = app.local_shm_map + app.local_shm_offset;

	memcpy(app.local_virt_shm, tmp, IPC_SHM_SIZE);
	munmap(app.local_shm_map, app.local_shm_offset + shm_size);
	close(app.mem_fd);

	sem_destroy(&app.sema);

	sample_info("exit\n");

	return err;
}
