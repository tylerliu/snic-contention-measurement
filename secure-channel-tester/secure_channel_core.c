/*
 * Copyright (c) 2021-2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of
 *       conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>

#include <doca_argp.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_comch_consumer.h>
#include <doca_comch_producer.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "common.h"

#include <utils.h>

#include "secure_channel_core.h"

#define MAX_MSG_SIZE 65535	   /* Max message size */
#define SLEEP_IN_NANOS (10 * 1000) /* Sample the connection every 10 microseconds  */
#define MAX_FASTPATH_TASKS 1024	   /* Maximum number of producer/consumer tasks to use */
#define CACHE_ALIGN 64		   /* Cache line alignment for producer/consumer performance */

#define NS_PER_SEC 1E9	   /* Nano-seconds per second */
#define NS_PER_MSEC 1E6	   /* Nano-seconds per millisecond */
#ifdef CLOCK_MONOTONIC_RAW /* Defined in glibc bits/time.h */
#define CLOCK_TYPE_ID CLOCK_MONOTONIC_RAW
#else
#define CLOCK_TYPE_ID CLOCK_MONOTONIC
#endif

DOCA_LOG_REGISTER(SECURE_CHANNEL::Core);
volatile bool force_quit = false;

/*
 * Helper to calculate and print statistics
 *
 * @start [in]: start time
 * @end [in]: end time
 * @processed_msgs [in]: number of processed messages
 * @msg_size [in]: size of each message
 * @role [in]: "Producer" or "Consumer"
 * @is_interval [in]: true if printing interval stats
 */
static void print_stats(struct timespec *start, struct timespec *end, uint32_t processed_msgs, uint32_t msg_size, const char *role, bool is_interval);

/*
 * Signal handler
 *
 * @signum [in]: Signal number
 */
void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit...", signum);
		force_quit = true;
	}
}

/* Local memory data for preparing and allocating doca_bufs */
struct local_memory_bufs {
	struct doca_dev *dev;		/* device associated with memory */
	struct doca_mmap *mmap;		/* mmap for registered memory */
	struct doca_buf_inventory *inv; /* inventory to assign doca_bufs */
	char *buf_data;			/* allocated data to reference in bufs */
};

/*
 * ARGP Callback - Handle messages number parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t messages_number_callback(void *param, void *config)
{
	struct sc_config *app_cfg = (struct sc_config *)config;
	int nb_send_msg = *(int *)param;

	/*
	 * If continuous mode is enabled (Host side), num_msgs can be 0.
	 * If not continuous, it must be > 0.
	 * Note: DPU ignores this check if determined by metadata later, but for arg parsing we need some rule.
	 */
	if (nb_send_msg < 0) {
		DOCA_LOG_ERR("Amount of messages to be sent by the client cannot be negative");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->send_msg_nb = nb_send_msg;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle continuous mode parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t continuous_callback(void *param, void *config)
{
	struct sc_config *app_cfg = (struct sc_config *)config;
	bool continuous = *(bool *)param;

	app_cfg->continuous = continuous;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle message size parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t message_size_callback(void *param, void *config)
{
	struct sc_config *app_cfg = (struct sc_config *)config;
	int send_msg_size = *(int *)param;

	if (send_msg_size < 1 || send_msg_size > MAX_MSG_SIZE) {
		DOCA_LOG_ERR("Received message size is not supported. Max is %u", MAX_MSG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->send_msg_size = send_msg_size;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle Comm Channel DOCA device PCI address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dev_pci_addr_callback(void *param, void *config)
{
	struct sc_config *cfg = (struct sc_config *)config;
	const char *dev_pci_addr = (char *)param;

	if (strnlen(dev_pci_addr, DOCA_DEVINFO_PCI_ADDR_SIZE) == DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strlcpy(cfg->cc_dev_pci_addr, dev_pci_addr, DOCA_DEVINFO_PCI_ADDR_SIZE);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle Comm Channel DOCA device representor PCI address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rep_pci_addr_callback(void *param, void *config)
{
	struct sc_config *cfg = (struct sc_config *)config;
	const char *rep_pci_addr = (char *)param;

	if (cfg->mode == SC_MODE_DPU) {
		if (strnlen(rep_pci_addr, DOCA_DEVINFO_REP_PCI_ADDR_SIZE) == DOCA_DEVINFO_REP_PCI_ADDR_SIZE) {
			DOCA_LOG_ERR("Entered device representor PCI address exceeding the maximum size of %d",
				     DOCA_DEVINFO_REP_PCI_ADDR_SIZE - 1);
			return DOCA_ERROR_INVALID_VALUE;
		}


		strlcpy(cfg->cc_dev_rep_pci_addr, rep_pci_addr, DOCA_DEVINFO_REP_PCI_ADDR_SIZE);
	} else {
		DOCA_LOG_ERR("Representor Device provided, but mode is not DPU");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle direction parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t direction_callback(void *param, void *config)
{
	struct sc_config *cfg = (struct sc_config *)config;
	const char *direction = (char *)param;

	if (strcasecmp(direction, "bidirectional") == 0) {
		cfg->direction = SC_DIR_BIDIRECTIONAL;
	} else if (strcasecmp(direction, "send") == 0) {
		cfg->direction = SC_DIR_SEND_ONLY;
	} else if (strcasecmp(direction, "recv") == 0) {
		cfg->direction = SC_DIR_RECV_ONLY;
	} else {
		DOCA_LOG_ERR("Invalid direction '%s'. Must be 'bidirectional', 'send', or 'recv'.", direction);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

void new_consumer_callback(struct doca_comch_event_consumer *event,
			   struct doca_comch_connection *comch_connection,
			   uint32_t id)
{
	struct cc_ctx *cfg = comch_utils_get_user_data(comch_connection);

	(void)event;

	cfg->consumer_id = id;
}

void expired_consumer_callback(struct doca_comch_event_consumer *event,
			       struct doca_comch_connection *comch_connection,
			       uint32_t id)
{
	/* Unused */
	(void)event;
	(void)comch_connection;
	(void)id;

	DOCA_LOG_INFO("Consumer expired (peer disconnected). Exiting...");
	force_quit = true;
}

void comch_recv_event_cb(struct doca_comch_event_msg_recv *event,
			 uint8_t *recv_buffer,
			 uint32_t msg_len,
			 struct doca_comch_connection *comch_connection)
{
	struct cc_ctx *cfg = comch_utils_get_user_data(comch_connection);
	struct metadata_msg *meta;

	(void)event;

	/* Only messages received should be of type metadata_msg */
	if (msg_len != sizeof(struct metadata_msg)) {
		DOCA_LOG_ERR("Invalid message length detected: %u", msg_len);
		// MOVE TO ERROR STATE - PERHAPS POPULATE A BAD VALUE IN MESSAGE FIELD
		cfg->expected_msgs = -1;
		return;
	}

	meta = (struct metadata_msg *)recv_buffer;
	DOCA_LOG_INFO("Received metadata from peer. Meta: Type=%d, Count=%u, Size=%u, Dir=%u", 
			meta->type, ntohl(meta->num_msgs), ntohl(meta->msg_size), ntohl(meta->direction));

	/* If an end message is received, set the expected messages back to 0(wait) */
	if (meta->type == END_MSG) {
		if (cfg->expected_msgs != 0) {
			DOCA_LOG_WARN("Expected %u messages but received END_MSG", cfg->expected_msgs);
			cfg->expected_msgs = 0;
		}
		cfg->peer_finished = true;
		return;
	}


	/* Mirror configuration for symmetric traffic (User requirement 3) */
	/* We update the context's config pointer - assuming DPU wants to send same amount/continuous */
	/* Note: This updates global app_cfg since cfg->cfg points to it */
	if (meta->type == START_MSG) {
		cfg->expected_msgs = ntohl(meta->num_msgs);
		cfg->expected_msg_size = ntohl(meta->msg_size);
		cfg->metadata_received = true;

		uint32_t received_count = ntohl(meta->num_msgs);
		
		if (cfg->cfg->mode == SC_MODE_DPU) {
			uint32_t dir = ntohl(meta->direction);
			if (dir == SC_DIR_SEND_ONLY) /* Host Sends */
				cfg->cfg->direction = SC_DIR_RECV_ONLY;
			else if (dir == SC_DIR_RECV_ONLY) /* Host Recvs */
				cfg->cfg->direction = SC_DIR_SEND_ONLY;
			else
				cfg->cfg->direction = SC_DIR_BIDIRECTIONAL;
			
			/* Set local parameters based on decided direction */
			if (cfg->cfg->direction == SC_DIR_SEND_ONLY) {
				cfg->expected_msgs = 0;
				cfg->cfg->send_msg_nb = received_count;
			} else if (cfg->cfg->direction == SC_DIR_RECV_ONLY) {
				cfg->expected_msgs = received_count;
				cfg->cfg->send_msg_nb = 0;
			} else {
				/* Bidirectional */
				cfg->expected_msgs = received_count;
				cfg->cfg->send_msg_nb = received_count;
			}
			
			cfg->cfg->send_msg_size = cfg->expected_msg_size;
		} else {
			/* Host logic: Direction already set by args */
			/* Host expected_msgs comes from DPU echo */
			/* In Single Direction mode, if Host is SEND_ONLY, DPU echo should say 0? */
			/* But DPU echo logic needs to be checked */
			/* Actually, Host shouldn't overwrite its send_msg_nb from DPU */
			/* Just set expected_msgs */
			cfg->expected_msgs = received_count;
		}

		/* If 0 messages (continuous) set expected_msgs to 0 */
		if (received_count == 0)
			cfg->expected_msgs = 0;

		DOCA_LOG_INFO("Metadata Received. Direction: %d, Count: %d", cfg->cfg->direction, received_count);
		cfg->metadata_received = true;
	} else {
		DOCA_LOG_WARN("Received unknown message type: %d", meta->type);
	}
}

/*
 * Helper function to tear down local memory allocated with prepare_local_memory()
 *
 * @local_mem [out]: local memory data to destroy
 */
static void destroy_local_memory(struct local_memory_bufs *local_mem)
{
	(void)doca_dev_close(local_mem->dev);
	free(local_mem->buf_data);
	(void)doca_mmap_destroy(local_mem->mmap);
	(void)doca_buf_inventory_destroy(local_mem->inv);
}

/*
 * Helper function to prepare local memory for use with doca_bufs and producer/consumer
 *
 * @local_mem [in]: struct of local memory data to be populated
 * @pci_addr [in]: address of device to associate memory with
 * @buf_len [in]: length of each buffer
 * @num_bufs [in]: total number of buffers required
 * @permissions [in]: bitwise combination of access flags - see enum doca_access_flag
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t prepare_local_memory(struct local_memory_bufs *local_mem,
					 const char *pci_addr,
					 size_t buf_len,
					 uint32_t num_bufs,
					 uint32_t permissions)
{
	size_t data_length = buf_len * num_bufs;
	size_t modulo;
	doca_error_t result;

	/* Open device to use for local memory registration */
	result = open_doca_device_with_pci(pci_addr, NULL, &local_mem->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open device %s: %s", pci_addr, doca_error_get_descr(result));
		return result;
	}

	/* Aligned_alloc requires the length to be a multiple of the alignment value so may need to pad up */
	modulo = data_length % CACHE_ALIGN;
	local_mem->buf_data =
		(char *)aligned_alloc(CACHE_ALIGN, data_length + (modulo == 0 ? 0 : CACHE_ALIGN - modulo));
	if (local_mem->buf_data == NULL) {
		DOCA_LOG_ERR("Failed allocate buffer memory of length: %lu", data_length);
		result = DOCA_ERROR_NO_MEMORY;
		goto close_dev;
	}

	result = doca_mmap_create(&local_mem->mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mmap: %s", doca_error_get_descr(result));
		goto free_data;
	}

	result = doca_mmap_set_permissions(local_mem->mmap, permissions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set permissions on mmap: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	result = doca_mmap_add_dev(local_mem->mmap, local_mem->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add device to mmap: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	result = doca_mmap_set_memrange(local_mem->mmap, local_mem->buf_data, data_length);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set memrange of mmap: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	result = doca_mmap_start(local_mem->mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	result = doca_buf_inventory_create(num_bufs, &local_mem->inv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create buffer inventory: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	result = doca_buf_inventory_start(local_mem->inv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start buffer inventory: %s", doca_error_get_descr(result));
		goto destroy_inv;
	}

	return DOCA_SUCCESS;

destroy_inv:
	(void)doca_buf_inventory_destroy(local_mem->inv);
destroy_mmap:
	(void)doca_mmap_destroy(local_mem->mmap);
free_data:
	free(local_mem->buf_data);
close_dev:
	(void)doca_dev_close(local_mem->dev);

	return result;
}

/*
 * Callback for successful send_task completion
 *
 * @task [in]: send_task that has completed
 * @task_user_data [in]: task user data
 * @ctx_user_data [in]: context user data
 */
static void send_task_completed_callback(struct doca_comch_producer_task_send *task,
					 union doca_data task_user_data,
					 union doca_data ctx_user_data)
{
	struct fast_path_ctx *producer_ctx = (struct fast_path_ctx *)ctx_user_data.ptr;
	doca_error_t result;

	(void)task_user_data;

	if (producer_ctx->state != FASTPATH_IN_PROGRESS)
		return;

	(producer_ctx->completed_msgs)++;

	/* Move to a stopping state once enough messages have been confirmed as sent */
	if (producer_ctx->completed_msgs == producer_ctx->total_msgs) {
		producer_ctx->state = FASTPATH_COMPLETE;
		return;
	}

	/* Stop sending if enough messages are currently in flight */
	if (producer_ctx->submitted_msgs == producer_ctx->total_msgs)
		return;

	result = doca_task_submit(doca_comch_producer_task_send_as_task(task));
	while (result == DOCA_ERROR_AGAIN) {
		result = doca_task_submit(doca_comch_producer_task_send_as_task(task));
	}

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit producer send task: %s", doca_error_get_descr(result));
		producer_ctx->state = FASTPATH_ERROR;
	}

	(producer_ctx->submitted_msgs)++;
}

/*
 * Callback for error on send_task completion
 *
 * @task [in]: send_task that has completed
 * @task_user_data [in]: task user data
 * @ctx_user_data [in]: context user data
 */
static void send_task_fail_callback(struct doca_comch_producer_task_send *task,
				    union doca_data task_user_data,
				    union doca_data ctx_user_data)
{
	struct fast_path_ctx *producer_ctx = (struct fast_path_ctx *)ctx_user_data.ptr;

	(void)task;
	(void)task_user_data;

	/* Task fail errors may occur if context is in stopping state - this is expect */
	if (producer_ctx->state == FASTPATH_COMPLETE)
		return;

	DOCA_LOG_ERR("Received a producer send task completion error");
	producer_ctx->state = FASTPATH_ERROR;
}

/*
 * Start a producer thread
 *
 * @context [in]: Input parameter
 * @return: NULL (dummy return because of pthread requirement)
 */
static void *run_producer(void *context)
{
	struct doca_comch_producer_task_send *task[MAX_FASTPATH_TASKS] = {0};
	struct cc_ctx *ctx = (struct cc_ctx *)context;
	struct fast_path_ctx producer_ctx = {0};
	union doca_data ctx_user_data = {0};
	struct doca_comch_producer *producer;
	struct local_memory_bufs local_mem;
	struct doca_pe *producer_pe;
	struct doca_buf *doca_buf;
	enum doca_ctx_states state;
	uint32_t total_msgs;
	uint32_t total_tasks;
	uint32_t msg_len;
	uint32_t max_cap;
	uint32_t i;
	doca_error_t result, tmp_result;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};

	/* Messages on producer are based on user input */
	total_msgs = ctx->cfg->send_msg_nb;
	msg_len = ctx->cfg->send_msg_size;

	/* If requested messages exceeds maximum tasks, tasks will be resubmitted in their completion callback */
	total_tasks = (total_msgs > MAX_FASTPATH_TASKS) ? MAX_FASTPATH_TASKS : total_msgs;
	if (total_tasks == 0) // Continuous mode support for initial allocation
		total_tasks = MAX_FASTPATH_TASKS;

	producer_ctx.total_msgs = total_msgs;

	/* Producer sends the same buffer repeatedly so only needs to allocate space for one */
	result =
		prepare_local_memory(&local_mem, ctx->cfg->cc_dev_pci_addr, msg_len, 1, DOCA_ACCESS_FLAG_PCI_READ_ONLY);
	if (result != DOCA_SUCCESS) {
		ctx->send_result->result = result;
		goto exit_thread;
	}

	/* Verify producer can support message size */
	result = doca_comch_producer_cap_get_max_buf_size(doca_dev_as_devinfo(local_mem.dev), &max_cap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query producer cap: %s", doca_error_get_descr(result));
		goto destroy_local_mem;
	}

	if (max_cap < msg_len) {
		DOCA_LOG_ERR("Producer does not support message size. Requested: %u, max: %u", msg_len, max_cap);
		result = DOCA_ERROR_INVALID_VALUE;
		goto destroy_local_mem;
	}

	result = doca_pe_create(&producer_pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create producer progress engine: %s", doca_error_get_descr(result));
		goto destroy_local_mem;
	}

	result = doca_comch_producer_create(ctx->comch_connection, &producer);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create producer: %s", doca_error_get_descr(result));
		goto destroy_pe;
	}

	result = doca_pe_connect_ctx(producer_pe, doca_comch_producer_as_ctx(producer));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect producer to pe: %s", doca_error_get_descr(result));
		goto destroy_producer;
	}

	result = doca_comch_producer_task_send_set_conf(producer,
							send_task_completed_callback,
							send_task_fail_callback,
							total_tasks);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure producer send tasks: %s", doca_error_get_descr(result));
		goto destroy_pe;
	}

	/* Add user data to update context from callbacks */
	ctx_user_data.ptr = &producer_ctx;
	result = doca_ctx_set_user_data(doca_comch_producer_as_ctx(producer), ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set producer user data: %s", doca_error_get_descr(result));
		goto destroy_pe;
	}

	result = doca_ctx_start(doca_comch_producer_as_ctx(producer));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start producer: %s", doca_error_get_descr(result));
		goto destroy_pe;
	}

	/* Allocate a single buffer from registered local memory */
	result = doca_buf_inventory_buf_get_by_data(local_mem.inv,
						    local_mem.mmap,
						    local_mem.buf_data,
						    msg_len,
						    &doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure producer send tasks: %s", doca_error_get_descr(result));
		goto stop_producer;
	}

	/*
	 * Wait on external consumer to come up.
	 * This is handled in the comch progress_engine.
	 */
	/*
	 * Wait on external consumer to come up.
	 * This is handled in the comch progress_engine.
	 */
	while (ctx->consumer_id == 0 && !force_quit) {
		nanosleep(&ts, &ts);
	}

	if (force_quit)
		goto stop_producer;

	producer_ctx.state = FASTPATH_IN_PROGRESS;

	if (clock_gettime(CLOCK_TYPE_ID, &producer_ctx.start_time) != 0)
		DOCA_LOG_ERR("Failed to get timestamp");

	/* Allocate and submit max number of tasks */
	for (i = 0; i < total_tasks; i++) {
		result = doca_comch_producer_task_send_alloc_init(producer,
								  doca_buf,
								  NULL,
								  0,
								  ctx->consumer_id,
								  &task[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate a producer task: %s", doca_error_get_descr(result));
			goto free_tasks;
		}

		/* May need to wait for a post_recv message before being able to send */
		result = doca_task_submit(doca_comch_producer_task_send_as_task(task[i]));
		while (result == DOCA_ERROR_AGAIN) {
			result = doca_task_submit(doca_comch_producer_task_send_as_task(task[i]));
		}

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit producer send task: %s", doca_error_get_descr(result));
			goto free_tasks;
		}

		(producer_ctx.submitted_msgs)++;
	}

	/* Progress until all messages have been sent or an error occurred */
	if (ctx->cfg->continuous || total_msgs == 0) {
		struct timespec start_interval, end_interval;
		uint32_t msgs_interval_start = 0;

		clock_gettime(CLOCK_TYPE_ID, &start_interval);

		while (!force_quit && producer_ctx.state == FASTPATH_IN_PROGRESS) {
			doca_pe_progress(producer_pe);
			
			/* Print stats every second */
			clock_gettime(CLOCK_TYPE_ID, &end_interval);
			if (end_interval.tv_sec > start_interval.tv_sec) {
				uint32_t current_msgs = producer_ctx.completed_msgs;
				print_stats(&start_interval, &end_interval, current_msgs - msgs_interval_start, msg_len, "Producer [Interval]", true);
				
				start_interval = end_interval;
				msgs_interval_start = current_msgs;
			}
		}
		producer_ctx.state = FASTPATH_COMPLETE; /* Force complete on exit */
	} else {
		while (producer_ctx.state == FASTPATH_IN_PROGRESS)
			doca_pe_progress(producer_pe);
	}

	if (clock_gettime(CLOCK_TYPE_ID, &producer_ctx.end_time) != 0)
		DOCA_LOG_ERR("Failed to get timestamp");

	if (producer_ctx.state == FASTPATH_ERROR) {
		result = DOCA_ERROR_BAD_STATE;
		DOCA_LOG_ERR("Producer datapath failed");
	}

free_tasks:
	/* Free all allocated tasks */
	for (i = 0; i < total_tasks; i++)
		if (task[i] != NULL)
			doca_task_free(doca_comch_producer_task_send_as_task(task[i]));

	doca_buf_dec_refcount(doca_buf, NULL);

stop_producer:
	tmp_result = doca_ctx_stop(doca_comch_producer_as_ctx(producer));
	if (tmp_result != DOCA_ERROR_IN_PROGRESS && tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop producer: %s", doca_error_get_descr(tmp_result));
		goto destroy_producer;
	}

	/* Wait for producer stop to complete */
	(void)doca_ctx_get_state(doca_comch_producer_as_ctx(producer), &state);
	while (state != DOCA_CTX_STATE_IDLE) {
		(void)doca_pe_progress(producer_pe);
		nanosleep(&ts, &ts);
		(void)doca_ctx_get_state(doca_comch_producer_as_ctx(producer), &state);
	}

destroy_producer:
	tmp_result = doca_comch_producer_destroy(producer);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy producer: %s", doca_error_get_descr(tmp_result));
destroy_pe:
	tmp_result = doca_pe_destroy(producer_pe);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy producer pe: %s", doca_error_get_descr(tmp_result));
destroy_local_mem:
	destroy_local_memory(&local_mem);

exit_thread:
	ctx->send_result->processed_msgs = producer_ctx.completed_msgs;
	ctx->send_result->start_time = producer_ctx.start_time;
	ctx->send_result->end_time = producer_ctx.end_time;
	ctx->send_result->result = result;

	atomic_fetch_sub(&ctx->active_threads, 1);

	return NULL;
}

/*
 * Callback for successful post_recv completion
 *
 * @task [in]: post_recv task that has completed
 * @task_user_data [in]: task user data
 * @ctx_user_data [in]: context user data
 */
static void recv_task_completed_callback(struct doca_comch_consumer_task_post_recv *task,
					 union doca_data task_user_data,
					 union doca_data ctx_user_data)
{
	struct fast_path_ctx *consumer_ctx = (struct fast_path_ctx *)ctx_user_data.ptr;
	struct doca_buf *buf;
	doca_error_t result;

	(void)task_user_data;

	/* Take timestamp of first message received */
	if (consumer_ctx->completed_msgs == 0) {
		if (clock_gettime(CLOCK_TYPE_ID, &consumer_ctx->start_time) != 0)
			DOCA_LOG_ERR("Failed to get timestamp");
	}

	(consumer_ctx->completed_msgs)++;

	if (consumer_ctx->completed_msgs == consumer_ctx->total_msgs)
		consumer_ctx->state = FASTPATH_COMPLETE;

	buf = doca_comch_consumer_task_post_recv_get_buf(task);

	/* Reset the buffer length so that it can be fully repopulated */
	result = doca_buf_reset_data_len(buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to reset doca_buf length: %s", doca_error_get_descr(result));
		consumer_ctx->state = FASTPATH_ERROR;
		return;
	}

	/* Resubmit post recv task */
	result = doca_task_submit(doca_comch_consumer_task_post_recv_as_task(task));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to resubmit post_recv task: %s", doca_error_get_descr(result));
		consumer_ctx->state = FASTPATH_ERROR;
	}
}

/*
 * Callback for error on post_recv completion
 *
 * @task [in]: post_recv task that has completed
 * @task_user_data [in]: task user data
 * @ctx_user_data [in]: context user data
 */
static void recv_task_fail_callback(struct doca_comch_consumer_task_post_recv *task,
				    union doca_data task_user_data,
				    union doca_data ctx_user_data)
{
	struct fast_path_ctx *consumer_ctx = (struct fast_path_ctx *)ctx_user_data.ptr;

	(void)task;
	(void)task_user_data;

	/* Task fail errors may occur if context is in stopping state - this is expect */
	if (consumer_ctx->state == FASTPATH_COMPLETE)
		return;

	DOCA_LOG_ERR("Received a consumer post recv completion error");
	consumer_ctx->state = FASTPATH_ERROR;
}

/*
 * Start a consumer thread
 *
 * @context [in]: Input parameter
 * @return: NULL (dummy return because of pthread requirement)
 */
static void *run_consumer(void *context)
{
	struct doca_comch_consumer_task_post_recv *task[MAX_FASTPATH_TASKS] = {0};
	struct cc_ctx *ctx = (struct cc_ctx *)context;
	struct doca_buf *doca_buf[MAX_FASTPATH_TASKS] = {0};
	struct doca_comch_consumer *consumer;
	struct fast_path_ctx consumer_ctx = {0};
	union doca_data ctx_user_data = {0};
	struct local_memory_bufs local_mem;
	struct doca_pe *consumer_pe;
	enum doca_ctx_states state;
	uint32_t total_msgs;
	uint32_t total_tasks;
	uint32_t msg_len;
	uint32_t max_cap;
	uint32_t i;
	doca_error_t result, tmp_result;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};

	/* Messages expected by consumer are based metadata received from opposite side */
	total_msgs = ctx->expected_msgs;
	msg_len = ctx->expected_msg_size;

	/* If expected receive messages exceeds maximum tasks, tasks will be reused as post_recv */
	total_tasks = (total_msgs > MAX_FASTPATH_TASKS) ? MAX_FASTPATH_TASKS : total_msgs;
	if (total_tasks == 0) // Continuous mode
		total_tasks = MAX_FASTPATH_TASKS;

	consumer_ctx.total_msgs = total_msgs;

	/* Consumer allocates a buffer of expected length for every task - must have write access */
	result = prepare_local_memory(&local_mem,
				      ctx->cfg->cc_dev_pci_addr,
				      msg_len,
				      total_tasks,
				      DOCA_ACCESS_FLAG_PCI_READ_WRITE);
	if (result != DOCA_SUCCESS) {
		ctx->recv_result->result = result;
		goto exit_thread;
	}

	/* Verify consumer can support message size */
	result = doca_comch_consumer_cap_get_max_buf_size(doca_dev_as_devinfo(local_mem.dev), &max_cap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query consumer cap: %s", doca_error_get_descr(result));
		goto destroy_local_mem;
	}

	if (max_cap < msg_len) {
		DOCA_LOG_ERR("Consumer does not support message size. Requested: %u, max: %u", msg_len, max_cap);
		result = DOCA_ERROR_INVALID_VALUE;
		goto destroy_local_mem;
	}

	result = doca_pe_create(&consumer_pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create consumer progress engine: %s", doca_error_get_descr(result));
		goto destroy_local_mem;
	}

	result = doca_comch_consumer_create(ctx->comch_connection, local_mem.mmap, &consumer);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create consumer: %s", doca_error_get_descr(result));
		goto destroy_pe;
	}

	result = doca_pe_connect_ctx(consumer_pe, doca_comch_consumer_as_ctx(consumer));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect consumer to pe: %s", doca_error_get_descr(result));
		goto destroy_consumer;
	}

	result = doca_comch_consumer_task_post_recv_set_conf(consumer,
							     recv_task_completed_callback,
							     recv_task_fail_callback,
							     total_tasks);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure consumer send tasks: %s", doca_error_get_descr(result));
		goto destroy_pe;
	}

	/* Add user data to update context from callbacks */
	ctx_user_data.ptr = &consumer_ctx;
	result = doca_ctx_set_user_data(doca_comch_consumer_as_ctx(consumer), ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set consumer user data: %s", doca_error_get_descr(result));
		goto destroy_pe;
	}

	result = doca_ctx_start(doca_comch_consumer_as_ctx(consumer));
	if (result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to start consumer: %s", doca_error_get_descr(result));
		goto destroy_pe;
	}

	/* Wait for consumer start to complete */
	(void)doca_ctx_get_state(doca_comch_consumer_as_ctx(consumer), &state);
	while (state != DOCA_CTX_STATE_RUNNING && !force_quit) {
		(void)doca_pe_progress(consumer_pe);
		nanosleep(&ts, &ts);
		(void)doca_ctx_get_state(doca_comch_consumer_as_ctx(consumer), &state);
	}

	if (force_quit)
		goto destroy_consumer;

	consumer_ctx.state = FASTPATH_IN_PROGRESS;

	/* Assign a buffer and submit a post_recv message for every available task */
	for (i = 0; i < total_tasks; i++) {
		result = doca_buf_inventory_buf_get_by_addr(local_mem.inv,
							    local_mem.mmap,
							    local_mem.buf_data + (i * msg_len),
							    msg_len,
							    &doca_buf[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate a consumer buf: %s", doca_error_get_descr(result));
			goto free_task_and_bufs;
		}

		result = doca_comch_consumer_task_post_recv_alloc_init(consumer, doca_buf[i], &task[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate a post recv task: %s", doca_error_get_descr(result));
			goto free_task_and_bufs;
		}

		result = doca_task_submit(doca_comch_consumer_task_post_recv_as_task(task[i]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit consumer post recv task: %s", doca_error_get_descr(result));
			goto free_task_and_bufs;
		}
	}

	/* Progress until all expected messages have been received or an error occurred */
	if (total_msgs == 0) {
		struct timespec start_interval, end_interval;
		uint32_t msgs_interval_start = 0;

		clock_gettime(CLOCK_TYPE_ID, &start_interval);

		while (!force_quit && consumer_ctx.state == FASTPATH_IN_PROGRESS) {
			doca_pe_progress(consumer_pe);

			/* Print stats every second */
			clock_gettime(CLOCK_TYPE_ID, &end_interval);
			if (end_interval.tv_sec > start_interval.tv_sec) {
				uint32_t current_msgs = consumer_ctx.completed_msgs;
				print_stats(&start_interval, &end_interval, current_msgs - msgs_interval_start, msg_len, "Consumer [Interval]", true);
				
				start_interval = end_interval;
				msgs_interval_start = current_msgs;
			}
		}
		consumer_ctx.state = FASTPATH_COMPLETE;
	} else {
		while (consumer_ctx.state == FASTPATH_IN_PROGRESS) {
			doca_pe_progress(consumer_pe);
		}
	}

	if (clock_gettime(CLOCK_TYPE_ID, &consumer_ctx.end_time) != 0)
		DOCA_LOG_ERR("Failed to get timestamp");

	if (consumer_ctx.state == FASTPATH_ERROR) {
		result = DOCA_ERROR_BAD_STATE;
		DOCA_LOG_ERR("Consumer datapath failed");
	}

free_task_and_bufs:
	/* Free all allocated buffers and tasks */
	for (i = 0; i < total_tasks; i++) {
		if (doca_buf[i] != NULL)
			doca_buf_dec_refcount(doca_buf[i], NULL);
		if (task[i] != NULL)
			doca_task_free(doca_comch_consumer_task_post_recv_as_task(task[i]));
	}

	tmp_result = doca_ctx_stop(doca_comch_consumer_as_ctx(consumer));
	if (tmp_result != DOCA_ERROR_IN_PROGRESS && tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop consumer: %s", doca_error_get_descr(tmp_result));
		goto destroy_consumer;
	}

	/* Wait for consumer stop to complete */
	(void)doca_ctx_get_state(doca_comch_consumer_as_ctx(consumer), &state);
	while (state != DOCA_CTX_STATE_IDLE) {
		(void)doca_pe_progress(consumer_pe);
		nanosleep(&ts, &ts);
		(void)doca_ctx_get_state(doca_comch_consumer_as_ctx(consumer), &state);
	}

destroy_consumer:
	tmp_result = doca_comch_consumer_destroy(consumer);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy consumer: %s", doca_error_get_descr(tmp_result));

destroy_pe:
	tmp_result = doca_pe_destroy(consumer_pe);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy consumer pe: %s", doca_error_get_descr(tmp_result));

destroy_local_mem:
	destroy_local_memory(&local_mem);

exit_thread:
	ctx->recv_result->processed_msgs = consumer_ctx.completed_msgs;
	ctx->recv_result->start_time = consumer_ctx.start_time;
	ctx->recv_result->end_time = consumer_ctx.end_time;
	ctx->recv_result->result = result;

	atomic_fetch_sub(&ctx->active_threads, 1);

	return NULL;
}

/*
 * Start threads and wait for them to finish
 *
 * @ctx [in]: Thread context
 * @comch_cfg [in]: Comch channel to progress on
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t start_threads(struct cc_ctx *ctx, struct comch_cfg *comch_cfg)
{
	doca_error_t result;
	int running_threads = 0;
	bool start_producer = (ctx->cfg->direction == SC_DIR_BIDIRECTIONAL || ctx->cfg->direction == SC_DIR_SEND_ONLY);
	bool start_consumer = (ctx->cfg->direction == SC_DIR_BIDIRECTIONAL || ctx->cfg->direction == SC_DIR_RECV_ONLY);
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};

	if (start_producer) running_threads++;
	if (start_consumer) running_threads++;

	DOCA_LOG_INFO("Starting threads: Producer=%s, Consumer=%s", start_producer ? "Yes" : "No", start_consumer ? "Yes" : "No");

	/* set count to determine when threads have finished */
	atomic_init(&ctx->active_threads, running_threads);

	if (start_producer) {
		if (pthread_create(ctx->sendto_t, NULL, run_producer, (void *)ctx) != 0) {
			DOCA_LOG_ERR("Failed to start sendto thread");
			return DOCA_ERROR_BAD_STATE;
		}

		if (pthread_detach(*ctx->sendto_t) != 0) {
			DOCA_LOG_ERR("Failed to detach sendto thread");
			return DOCA_ERROR_BAD_STATE;
		}
	}

	if (start_consumer) {
		if (pthread_create(ctx->recvfrom_t, NULL, run_consumer, (void *)ctx) != 0) {
			DOCA_LOG_ERR("Failed to start recvfrom thread");
			return DOCA_ERROR_BAD_STATE;
		}

		if (pthread_detach(*ctx->recvfrom_t) != 0) {
			DOCA_LOG_ERR("Failed to detach recvfrom thread");
			return DOCA_ERROR_BAD_STATE;
		}
	}

	/*
	 * Progress the comch PE while waiting for the threads to finish.
	 * Comch handles producer and consumer control messages so must continue to run.
	 */
	while (running_threads > 0) {
		if (comch_util_get_connection(comch_cfg) != NULL) {
			result = comch_utils_progress_connection(comch_util_get_connection(comch_cfg));
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Error in comch progression: %s. Signaling threads to exit.", doca_error_get_descr(result));
				force_quit = true;
			}
		} else {
			/* Connection lost, wait for threads */
			nanosleep(&ts, &ts);
		}
		
		/* If force_quit is set elsewhere (e.g. signal), ensure threads know */
		/* Threads check force_quit global */

		running_threads = atomic_load(&ctx->active_threads);
	}

	return DOCA_SUCCESS;
}

/*
 * Helper to calculate time difference between two timespec structs
 *
 * @end [in]: end time
 * @start [in]: start time
 * @return: time difference in milliseconds
 */
static double calculate_timediff_ms(struct timespec *end, struct timespec *start)
{
	long diff;

	diff = (end->tv_sec - start->tv_sec) * NS_PER_SEC;
	diff += end->tv_nsec;
	diff -= start->tv_nsec;

	return (double)(diff / NS_PER_MSEC);
}

/*
 * Helper to calculate and print statistics
 *
 * @start [in]: start time
 * @end [in]: end time
 * @processed_msgs [in]: number of processed messages
 * @msg_size [in]: size of each message
 * @role [in]: "Producer" or "Consumer"
 * @is_interval [in]: true if printing interval stats
 */
static void print_stats(struct timespec *start, struct timespec *end, uint32_t processed_msgs, uint32_t msg_size, const char *role, bool is_interval)
{
	double duration_ms = calculate_timediff_ms(end, start);
	double duration_sec = duration_ms / 1000.0;
	double throughput = processed_msgs / duration_sec;
	double bandwidth_mb = (double)processed_msgs * msg_size / (1024 * 1024) / duration_sec;
	const char *verb = (strncmp(role, "Consumer", 8) == 0) ? "received" : "sent";

	if (!is_interval)
		DOCA_LOG_INFO("%s %s %u messages in approximately %.4f milliseconds", role, verb, processed_msgs, duration_ms);
	DOCA_LOG_INFO("%s Throughput: %.2f Msgs/s", role, throughput);
	DOCA_LOG_INFO("%s Bandwidth: %.2f MB/s", role, bandwidth_mb);
}

doca_error_t sc_start(struct comch_cfg *comch_cfg, struct sc_config *cfg, struct cc_ctx *ctx)
{
	struct t_results send_result = {0};
	struct t_results recv_result = {0};
	pthread_t sendto_thread, recvfrom_thread;
	doca_error_t result;
	struct metadata_msg meta = {0};

	ctx->comch_connection = comch_util_get_connection(comch_cfg);
	ctx->cfg = cfg;

	if (cfg->mode == SC_MODE_DPU) {
		DOCA_LOG_INFO("DPU Mode detected. Waiting for Host metadata...");
		/* DPU Wait first to receive configuration from Host */
		while (!ctx->metadata_received && !force_quit) {
			result = comch_utils_progress_connection(comch_util_get_connection(comch_cfg));
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to progress comch: %s", doca_error_get_descr(result));
				return result;
			}
		}
		DOCA_LOG_INFO("Exited DPU wait loop. Received=%d, Quit=%d", ctx->metadata_received, force_quit);
	}

	if (force_quit)
		return DOCA_SUCCESS;

	/* Send a comch metadata message to the other side indicating the number of fastpath messages */
	meta.type = START_MSG;
	meta.num_msgs = htonl(cfg->send_msg_nb);
	meta.msg_size = htonl(cfg->send_msg_size);
	meta.direction = htonl(cfg->direction);
	
	DOCA_LOG_INFO("Sending START_MSG: Type=%d, Count=%d, Size=%d, Dir=%d", 
		meta.type, cfg->send_msg_nb, cfg->send_msg_size, cfg->direction);

	result = comch_utils_send(comch_util_get_connection(comch_cfg), &meta, sizeof(struct metadata_msg));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send metadata message: %s", doca_error_get_descr(result));
		return result;
	}

	if (cfg->mode != SC_MODE_DPU) {
		/* Host Wait for DPU to echo/ack */
		while (!ctx->metadata_received && !force_quit) {
			result = comch_utils_progress_connection(comch_util_get_connection(comch_cfg));
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to progress comch: %s", doca_error_get_descr(result));
				return result;
			}
		}
	}

	if (force_quit)
		return DOCA_SUCCESS;

	if (ctx->expected_msgs < 0) {
		DOCA_LOG_ERR("Got a bad metadata message on comch");
		return DOCA_ERROR_INVALID_VALUE;
	}

	ctx->sendto_t = &sendto_thread;
	ctx->recvfrom_t = &recvfrom_thread;
	ctx->send_result = &send_result;
	ctx->recv_result = &recv_result;

	result = start_threads(ctx, comch_cfg);
	if (result != DOCA_SUCCESS) {
		return result;
	}

	/* Check results only for active threads */
	bool check_producer = (cfg->direction == SC_DIR_BIDIRECTIONAL || cfg->direction == SC_DIR_SEND_ONLY);
	bool check_consumer = (cfg->direction == SC_DIR_BIDIRECTIONAL || cfg->direction == SC_DIR_RECV_ONLY);

	if (check_producer) {
		result = ctx->send_result->result;
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Send thread finished unsuccessfully");
			return result;
		}
	}

	if (check_consumer) {
		result = ctx->recv_result->result;
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Receive thread finished unsuccessfully");
			return result;
		}
	}

	/*
	 * To ensure that both sides have finished with the comch channel send an end message from DPU to host.
	 * On the host side, wait to receive said message (sets expected msgs back to 0).
	 * Comch utils enforces that the client must disconnect from the server before it can be destroyed.
	 */
	if (cfg->mode == SC_MODE_DPU) {
		if (!force_quit) {
			meta.type = END_MSG;
			DOCA_LOG_INFO("Sending END_MSG.");
			result = comch_utils_send(comch_util_get_connection(comch_cfg), &meta, sizeof(struct metadata_msg));
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to send metadata message: %s", doca_error_get_descr(result));
				/* Don't return, proceed to stats */
			}
		}
	} else {
		DOCA_LOG_INFO("Waiting for peer to finish (END_MSG)...");
		while (!ctx->peer_finished && !force_quit) {
			result = comch_utils_progress_connection(comch_util_get_connection(comch_cfg));
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to progress comch: %s", doca_error_get_descr(result));
				return result;
			}
		}
		DOCA_LOG_INFO("Peer finished.");
	}

	if (check_producer)
		print_stats(&ctx->send_result->start_time, &ctx->send_result->end_time, ctx->send_result->processed_msgs, cfg->send_msg_size, "Producer [Total]", false);
	if (check_consumer)
		print_stats(&ctx->recv_result->start_time, &ctx->recv_result->end_time, ctx->recv_result->processed_msgs, ctx->expected_msg_size, "Consumer [Total]", false);

	return result;
}

doca_error_t register_secure_channel_params(void)
{
	doca_error_t result;

	struct doca_argp_param *message_size_param, *messages_number_param, *pci_addr_param, *rep_pci_addr_param, *continuous_param, *direction_param;

	/* Create and register continuous mode param */
	result = doca_argp_param_create(&continuous_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(continuous_param, "c");
	doca_argp_param_set_long_name(continuous_param, "continuous");
	doca_argp_param_set_description(continuous_param, "Run in continuous mode (Host only)");
	doca_argp_param_set_callback(continuous_param, continuous_callback);
	doca_argp_param_set_type(continuous_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(continuous_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register direction param */
	result = doca_argp_param_create(&direction_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(direction_param, "d");
	doca_argp_param_set_long_name(direction_param, "direction");
	doca_argp_param_set_description(direction_param, "Traffic direction: 'bidirectional', 'send', 'recv' (Host only, default: bidirectional)");
	doca_argp_param_set_callback(direction_param, direction_callback);
	doca_argp_param_set_type(direction_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(direction_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register message to send param */
	result = doca_argp_param_create(&message_size_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(message_size_param, "s");
	doca_argp_param_set_long_name(message_size_param, "msg-size");
	doca_argp_param_set_description(message_size_param, "Message size to be sent");
	doca_argp_param_set_callback(message_size_param, message_size_callback);
	doca_argp_param_set_type(message_size_param, DOCA_ARGP_TYPE_INT);
	/* doca_argp_param_set_mandatory(message_size_param); - Optional for DPU */
	result = doca_argp_register_param(message_size_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register number of message param */
	result = doca_argp_param_create(&messages_number_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(messages_number_param, "n");
	doca_argp_param_set_long_name(messages_number_param, "num-msgs");
	doca_argp_param_set_description(messages_number_param, "Number of messages to be sent");
	doca_argp_param_set_callback(messages_number_param, messages_number_callback);
	doca_argp_param_set_type(messages_number_param, DOCA_ARGP_TYPE_INT);
	/* Mandatory only if not continuous - but argp doesn't support conditional mandatory. We check logically. */
	/* doca_argp_param_set_mandatory(messages_number_param); */
	result = doca_argp_register_param(messages_number_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register Comm Channel DOCA device PCI address */
	result = doca_argp_param_create(&pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(pci_addr_param, "p");
	doca_argp_param_set_long_name(pci_addr_param, "pci-addr");
	doca_argp_param_set_description(pci_addr_param, "DOCA Comch device PCI address");
	doca_argp_param_set_callback(pci_addr_param, dev_pci_addr_callback);
	doca_argp_param_set_type(pci_addr_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(pci_addr_param);
	result = doca_argp_register_param(pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register Comm Channel DOCA device representor PCI address */
	result = doca_argp_param_create(&rep_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(rep_pci_addr_param, "r");
	doca_argp_param_set_long_name(rep_pci_addr_param, "rep-pci");
	doca_argp_param_set_description(rep_pci_addr_param,
					"DOCA Comch device representor PCI address (needed only on DPU)");
	doca_argp_param_set_callback(rep_pci_addr_param, rep_pci_addr_callback);
	doca_argp_param_set_type(rep_pci_addr_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(rep_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Register version callback for DOCA SDK & RUNTIME */
	result = doca_argp_register_version_callback(sdk_version_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register version callback: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}
