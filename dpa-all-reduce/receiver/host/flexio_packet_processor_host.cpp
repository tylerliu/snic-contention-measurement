/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES.
 * Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Used for geteuid function. */
#include <cstdio>
#include <unistd.h>
#include <chrono>

/* Used for host (x86/DPU) memory allocations. */
#include <malloc.h>
#include <getopt.h>
#include <vector>
#include <cmath>

/* Used for IBV device operations. */
#include <infiniband/mlx5dv.h>

/* Flex IO SDK host side version API header. */
#include <libflexio/flexio_ver.h>

/* Set current version of FLEXIO_VER_USED. */
#define FLEXIO_VER_USED FLEXIO_VER(25, 7, 0)

/* Flex IO SDK host side API header. */
#include <libflexio/flexio.h>

/* Flow steering utilities helper header. */
#include "flow_steering_utils.h"
#include "flexio_queue_wrappers.h"

/* Common header for communication between host and DPA. */
#include "../flexio_packet_processor_com.h"

// DPA module is defined with C linkage. 
extern "C" {
extern flexio_func_t flexio_pp_dev;
extern flexio_func_t dpa_get_total_packets;
extern flexio_func_t dpa_get_total_bytes;
}

/* Application context struct holding necessary host side variables. */
/* Per-thread resources */
struct ThreadResources {
	int thread_id;

	/* Flex IO event handler is used to execute code over the DPA. */
	struct flexio_event_handler *pp_eh;
	
	/* Flex IO RQ wrapper */
	std::unique_ptr<FlexioRQ> flexio_rq;

	/* RX flow rule for matching incoming RX packets to the Flex IO RQ. */
	std::unique_ptr<FlowRule> rx_rule;

	/* Transfer structs with information to pass to DPA side. */
	struct app_transfer_cq rq_cq_transf;
	struct app_transfer_wq rq_transf;

	/* DPA heap memory address of application information struct. */
	flexio_uintptr_t app_data_daddr;
};

/* Application context struct holding necessary host side variables. */
struct app_context {
	/* Flex IO process is used to load a program to the DPA. */
	struct flexio_process *flexio_process;
	/* Flex IO application to load to the process. */
	struct flexio_app *flexio_app;
	/* Flex IO message stream is used to get messages from the DPA. */
	struct flexio_msg_stream *stream;

	/* Vector of per-thread resources */
	std::vector<std::unique_ptr<ThreadResources>> threads;

	/* DPA user access register (DPA UAR) for all application's queues.
	 * Will be set to the Flex IO process UAR.
	 */
	struct flexio_uar *process_uar;

	/* Protection domain (PD) for all application's queues.
	 * Will be set to the Flex IO process PD.
	 */
	struct ibv_pd *process_pd;
	/* IBV context opened for the device name provided by the user. */
	struct ibv_context *ibv_ctx;

	/* RX flow matcher. */
	std::unique_ptr<FlowMatcher> rx_matcher;

	/* Device MAC Address */
	uint8_t device_mac[6];
	/* Verbose mode */
	uint8_t verbose;

	/* DPA heap memory address of application information struct. */
	flexio_uintptr_t app_data_daddr;

	/* Run RQ ring on Host/ARM memory */
	bool rq_on_host;
	/* Flex IO Window for accessing host memory */
	struct flexio_window *window;
};



/* Open ibv device
 * Returns 0 on success and -1 if the destroy was failed.
 * app_ctx - app_ctx - pointer to app_context structure.
 * device - device name to open.
 */
static int app_open_ibv_ctx(struct app_context *app_ctx, char *device)
{
	/* Queried IBV device list. */
	struct ibv_device **dev_list;
	/* Function return value. */
	int ret = 0;
	/* IBV device iterator. */
	int dev_i;

	/* Query IBV devices list. */
	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		printf("Failed to get IB devices list\n");
		return -1;
	}

	/* Loop over found IBV devices. */
	for (dev_i = 0; dev_list[dev_i]; dev_i++) {
		/* Look for a device with the user provided name. */
		if (!strcmp(ibv_get_device_name(dev_list[dev_i]), device))
			break;
	}

	/* Check a device was found. */
	if (!dev_list[dev_i]) {
		printf("No IBV device found for device name '%s'\n", device);
		ret = -1;
		goto cleanup;
	}

	/* Open IBV device context for the requested device. */
	app_ctx->ibv_ctx = ibv_open_device(dev_list[dev_i]);
	if (!app_ctx->ibv_ctx) {
		printf("Couldn't open an IBV context for device '%s'\n", device);
		ret = -1;
	}

cleanup:
	/* Free queried IBV devices list. */
	ibv_free_device_list(dev_list);

	return ret;
}

/* Convert logarithm to value. */
#define L2V(l) (1UL << (l))
/* Number of entries in each RQ/SQ/CQ is 2^LOG_Q_DEPTH. */
#define LOG_Q_DEPTH 7
#define Q_DEPTH L2V(LOG_Q_DEPTH)
/* SQ/RQ data entry byte size is 512B (enough for packet data in this case). */
#define LOG_Q_DATA_ENTRY_BSIZE 11
/* SQ/RQ data entry byte size log to value. */
#define Q_DATA_ENTRY_BSIZE L2V(LOG_Q_DATA_ENTRY_BSIZE)
/* SQ/RQ DATA byte size is queue depth times entry byte size. */
#define Q_DATA_BSIZE Q_DEPTH *Q_DATA_ENTRY_BSIZE

/* Creates an MKey with proper permissions for access from DPA.
 * For this application, we only need memory write access.
 * Returns pointer to flexio_mkey structure on success. Otherwise, returns NULL.
 * app_ctx - pointer to app_context structure.
 * daddr - address of MKEY data.
 */


/* Creates steering rules for application.
 * Returns 0 on success and -1 if the allocation was failed.
 * app_ctx - pointer to app_context structure.
 * nic_mode - if set to 1, the sample runs on ConnectX part.
 * udp_port - destination UDP port to match.
 * device_mac - Source MAC address of the device (for TX matching).
 */
/* Creates steering rules for application.
 * Returns 0 on success and -1 if the allocation was failed.
 * app_ctx - pointer to app_context structure.
 * thread - pointer to thread resources.
 * nic_mode - if set to 1, the sample runs on ConnectX part.
 * udp_port - destination UDP port to match.
 * device_mac - Source MAC address of the device (for TX matching).
 */
static int create_steering_rules(struct app_context *app_ctx, struct ThreadResources *thread, uint16_t udp_dport, uint16_t udp_sport)
{
	/* Create RX flow rule for matching IPv4 packets. */
	thread->rx_rule =
		FlowRule::create_rx_udp_port_match(app_ctx->rx_matcher.get(),
					flexio_rq_get_tir(thread->flexio_rq->get_rq()), udp_dport, udp_sport);
	printf("Thread %d TIR: %p, DPort: %d, SPort: %d\n", thread->thread_id, flexio_rq_get_tir(thread->flexio_rq->get_rq()), udp_dport, udp_sport);
	if (!thread->rx_rule) {
		printf("Failed to create RX steering rule\n");
		return -1;
	}

	return 0;
}

/* Create an SQ over the DPA for sending packets from DPA to wire.
 * A CQ is also created for the SQ.
 * Returns 0 on success and -1 if the allocation fails.
 * app_ctx - app_ctx - pointer to app_context structure.
 */
/* Create an SQ over the DPA for sending packets from DPA to wire.
 * A CQ is also created for the SQ.
 * Returns 0 on success and -1 if the allocation fails.
 * app_ctx - pointer to app_context structure.
 * thread - pointer to thread resources.
 */


/* Create an RQ over the DPA for receiving packets on DPA.
 * A CQ is also created for the RQ.
 * Returns 0 on success and -1 if the allocation fails.
 * app_ctx - app_ctx - pointer to app_context structure.
 */
/* Create an RQ over the DPA for receiving packets on DPA.
 * A CQ is also created for the RQ.
 * Returns 0 on success and -1 if the allocation fails.
 * app_ctx - pointer to app_context structure.
 * thread - pointer to thread resources.
 */
static int create_app_rq(struct app_context *app_ctx, struct ThreadResources *thread)
{
    try {
        thread->flexio_rq = std::make_unique<FlexioRQ>(app_ctx->flexio_process,
                                                        app_ctx->process_uar,
                                                        thread->pp_eh,
                                                        LOG_Q_DEPTH,
                                                        app_ctx->process_pd,
                                                        app_ctx->rq_on_host);
    } catch (const std::exception& e) {
        printf("Failed to create Flex IO RQ wrapper: %s\n", e.what());
        return -1;
    }

	/* Populate transfer struct */
	thread->rq_cq_transf.cq_num = thread->flexio_rq->get_cq()->get_cq_num();
	thread->rq_cq_transf.log_cq_depth = LOG_Q_DEPTH;
	thread->rq_cq_transf.cq_dbr_daddr = thread->flexio_rq->get_cq()->get_dbr_daddr();
	thread->rq_cq_transf.cq_ring_daddr = thread->flexio_rq->get_cq()->get_ring_daddr();

	thread->rq_transf.wq_num = thread->flexio_rq->get_wq_num();
	thread->rq_transf.wqd_mkey_id = thread->flexio_rq->get_wqd_mkey_id();
	thread->rq_transf.wqd_daddr = thread->flexio_rq->get_wqd_daddr();
	thread->rq_transf.wq_ring_daddr = thread->flexio_rq->get_wq_ring_daddr();
	thread->rq_transf.wq_dbr_daddr = thread->flexio_rq->get_wq_dbr_daddr();

	return 0;
}

/* Creates a Flex IO SDK event handler.
 * The event handler is used for setting a function in the loaded program to run once
 * a proper trigger happens (CQE on the relevant CQ).
 * Returns 0 on success and -1 if the allocation fails.
 * app_ctx - pointer to app_context structure.
 * thread - pointer to thread resources.
 */
static int create_app_event_handler(struct app_context *app_ctx, struct ThreadResources *thread)
{
	/* Event handler creation attributes. */
	struct flexio_event_handler_attr eh_attr = {0};

	/* Set function stub to the stub created by DPACC and declared in the host application. */
	eh_attr.host_stub_func = flexio_pp_dev;
	/* Set execution unit affinity to 'none'.
	 * This will cause the event handler thread to trigger on any free execution unit.
	 * This assumes there's at least one available execution unit in the device default
	 * execution unit group.
	 */
	eh_attr.affinity.type = FLEXIO_AFFINITY_NONE;
	/* Create the Flex IO event handler object. */
	if (flexio_event_handler_create(app_ctx->flexio_process, &eh_attr, &thread->pp_eh)) {
		printf("Failed to create Flex IO event handler\n");
		return -1;
	}

	return 0;
}

/* Copy application information to DPA.
 * DPA side needs queue information in order to process the packets.
 * The DPA heap memory address will be passed as the event handler argument.
 * Returns 0 if success and -1 if the copy failed.
 * app_ctx - pointer to app_context structure.
 * thread - pointer to thread resources.
 */
static int copy_app_data_to_dpa(struct app_context *app_ctx, struct ThreadResources *thread)
{
	/* Size of application information struct. */
	uint64_t struct_bsize = sizeof(struct host2dev_packet_processor_data);
	/* Temporary application information struct to copy. */
	struct host2dev_packet_processor_data *h2d_data;
	/* Function return value. */
	int ret = 0;

	/* Allocate memory for temporary struct to copy. */
	h2d_data = (struct host2dev_packet_processor_data *) calloc(1, struct_bsize);
	if (!h2d_data) {
		printf("Failed to allocate memory for h2d_data\n");
		return -1;
	}

	/* Set RQ's CQ information. */
	h2d_data->rq_cq_transf = thread->rq_cq_transf;
	/* Set RQ's information. */
	h2d_data->rq_transf = thread->rq_transf;
	/* Set APP data info for first run. */
	h2d_data->not_first_run = 0;
	/* Set device MAC address. */
	memcpy(h2d_data->device_mac, app_ctx->device_mac, sizeof(app_ctx->device_mac));
	h2d_data->verbose = app_ctx->verbose;
	h2d_data->rq_on_host = app_ctx->rq_on_host ? 1 : 0;
	if (app_ctx->rq_on_host && app_ctx->window) {
		h2d_data->rq_window_id = flexio_window_get_id(app_ctx->window);
	} else {
		h2d_data->rq_window_id = 0;
	}
	h2d_data->thread_id = thread->thread_id;

	/* Copy to DPA heap memory.
	 * Allocated DPA heap memory address will be kept in app_data_daddr.
	 */
	if (flexio_copy_from_host(app_ctx->flexio_process, h2d_data, struct_bsize,
				  &thread->app_data_daddr)) {
		printf("Failed to copy application information to DPA.\n");
		ret = -1;
	}

	/* Free temporary host memory. */
	free(h2d_data);
	return ret;
}

/* Clean up previously allocated rules.
 * app_ctx - pointer to app_context structure.
 */
static void clean_up_rules(struct app_context *app_ctx)
{
	for (auto& thread : app_ctx->threads) {
		thread->rx_rule.reset();
	}
	app_ctx->rx_matcher.reset();
}

/* dev msg stream buffer built from chunks of 2^FLEXIO_MSG_DEV_LOG_DATA_CHUNK_BSIZE each */
#define MSG_HOST_BUFF_BSIZE (512 * L2V(FLEXIO_MSG_DEV_LOG_DATA_CHUNK_BSIZE))

/* Application name in string format for Flex IO app get. */
#define DEV_APP_NAME_STR(_app_name) #_app_name
#define DEV_APP_NAME_XSTR(_app_name) DEV_APP_NAME_STR(_app_name)

/* Main host side function.
 * Responsible for allocating resources and making preparations for DPA side invocation.
 */
int main(int argc, char **argv)
{
	/* Flex IO app get selection attributes. */
	struct flexio_app_select_attr flexio_app_sel_attr = {0};
	/* Message stream attributes. */
	struct flexio_msg_stream_attr stream_fattr = {0};
	/* Pointer to the application Flex IO process (ease of use). */
	struct flexio_process *app_fp = NULL;
	/* Application context. */
	struct app_context app_ctx = {0};
	/* IBV port attributes. */
	struct ibv_port_attr port_attr;
	/* Debug token */
	uint64_t udbg_token;

	/* RQ on host (ARM) memory */
	int arm_mem = 0;
	/* Number of threads */
	int num_threads = 1;
	/* UDP port to match (default 0 - match all? No, user wants UDP match so maybe default to 0 means we should filter by 0? But 0 is reserved.
	 * If user doesn't specify port, maybe we can assume a default port or error.
	 * I'll initialize to 0. If user passes 0 or nothing, we pass 0 to matching logic.
	 * Wait, if I pass 0 to `udp_dport` match value with 0xffff mask, it will match packets with port 0.
	 * But the user might want "any port" if not specified.
	 * However, the user request says "add a match for udp port". "Make the port configurable".
	 * If I hardcode mask to 0xffff, I MUST provide a value.
	 * I will just set a default like 4791 (RoCE) if not specified, or 0.
	 * Let's set default to 1234 for now to be safe, or just 0.
	 */
	uint16_t udp_port = 1234;

	/* Execution status value. */
	int err;
	/* Device MAC address */
	uint8_t dev_mac[6] = {0x02, 0x42, 0x7e, 0x7f, 0xeb, 0x02}; /* Default to previous hardcoded TG_MAC */
	uint64_t device_mac_val = 0;
	/* Verbose mode */
	int verbose = 0;

	printf("Welcome to 'Flex IO SDK packet processing' sample app.\n");

	/* Parse command line arguments */
	static struct option long_options[] = {

		{"arm-mem", no_argument, 0, 'a'},
		{"port", required_argument, 0, 'p'},
		{"mac", required_argument, 0, 'm'},
		{"verbose", no_argument, 0, 'v'},
		{"threads", required_argument, 0, 't'},
		{"sport", required_argument, 0, 's'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};
	int c;
	char *device_name = NULL;

	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "nhp:m:vat:s:", long_options, &option_index);

		if (c == -1)
			break;

		switch (c) {

		case 'a':
			arm_mem = 1;
			break;
		case 'p':
			udp_port = atoi(optarg);
			break;
		case 'm':
			if (sscanf(optarg, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
				   &dev_mac[0], &dev_mac[1], &dev_mac[2],
				   &dev_mac[3], &dev_mac[4], &dev_mac[5]) != 6) {
				printf("Invalid MAC address format. Use xx:xx:xx:xx:xx:xx\n");
				return -1;
			}
			break;
		case 'v':
			verbose = 1;
			break;
		case 't':
			num_threads = atoi(optarg);
			if (num_threads < 1) {
				printf("Invalid number of threads. Must be >= 1\n");
				return -1;
			}
			if (num_threads > MAX_THREADS) {
				printf("Number of threads exceeds maximum supported (%d)\n", MAX_THREADS);
				return -1;
			}
			if (num_threads > MAX_THREADS) {
				printf("Number of threads exceeds maximum supported (%d)\n", MAX_THREADS);
				return -1;
			}
			break;
		case 's':
			/* Ignoring sport arg for now as we auto-calculate it, or maybe use it as base? 
			 * User request says "use source port to differentiate ... based on the last ... bits".
			 * It implies we steer based on Source Port % (2^N). 
			 * So we probably don't need a specific sport argument for matching, just the mask and value.
			 */
			break;
		case 'h':
			printf("Usage: %s [options] <mlx5 device>\n", argv[0]);
			printf("Options:\n");

			printf("  -a, --arm-mem        Allocate RQ ring on Host/ARM memory\n");
			printf("  -p, --port <port>    UDP Destination port to match (default 1234)\n");
			printf("  -m, --mac <mac>      Source MAC address (default 02:42:7e:7f:eb:02)\n");
			printf("  -v, --verbose        Enable verbose output (per-packet printing)\n");
			printf("  -t, --threads        Number of DPA threads to spawn (default 1)\n");
			printf("  -h, --help           Show this help message\n");
			return 0;
		default:
			printf("Invalid option. Use -h for help.\n");
			return -1;
		}
	}

	if (optind < argc) {
		device_name = argv[optind];
	} else {
		printf("Missing device name. Usage: %s <mlx5 device> [options]\n", argv[0]);
		return -1;
	}

	/* Check if the application run with root privileges */
	if (geteuid()) {
		printf("Failed - the application must run with root privileges\n");
		return -1;
	}

	/* Create an IBV device context by opening the provided IBV device. */
	err = app_open_ibv_ctx(&app_ctx, device_name);
	if (err)
		return -1;

	/* Get Device MAC */
	printf("Device MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
	       dev_mac[0], dev_mac[1], dev_mac[2], dev_mac[3], dev_mac[4], dev_mac[5]);

	memcpy(app_ctx.device_mac, dev_mac, sizeof(dev_mac));
	for (int i = 0; i < 6; i++)
		device_mac_val = (device_mac_val << 8) | dev_mac[i];
	app_ctx.verbose = verbose;
	app_ctx.rq_on_host = (arm_mem == 1);

	/* Retrieve the attributes of the device port */
	if (ibv_query_port(app_ctx.ibv_ctx, 1, &port_attr)) {
		printf("Failed to query IBV port attributes\n");
		err = -1;
		goto cleanup;
	}

	/* Check if the device is a valid Ethernet device. */
	if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET) {
		printf("IBV port is not Ethernet, state: %d\n", port_attr.link_layer);
		err = -1;
		goto cleanup;
	}

	/* Set current version for API. */
	if (flexio_version_set(FLEXIO_VER_USED)) {
		printf("Failed to set version in FlexIO API.\n");
		err = -1;
		goto cleanup;
	}

	/* Get Flex IO application struct for used device. */
	/* Set app name to match. */
	flexio_app_sel_attr.app_name = DEV_APP_NAME_XSTR(DEV_APP_NAME);
	/* Set HW platform to default - this will auto-select the appropriate program. */
	flexio_app_sel_attr.hw_model_id = FLEXIO_HW_MODEL_DEF;
	/* Set IBV device to use for HW model query. */
	flexio_app_sel_attr.ibv_ctx = app_ctx.ibv_ctx;

	/* Get a Flex IO application.
	 * DPACC created Flex IO application per HW model. Match the select attributes to the
	 * found applications and return the matching one. Name must match. HW model is matched
	 * According to the queried HW model for the device. If no exact match is found a program
	 * built to an older HW model will be selected.
	 */
	err = flexio_app_get(&flexio_app_sel_attr, &app_ctx.flexio_app);
	if (err) {
		printf("Failed to get Flex IO app\n");
		goto cleanup;
	}

	/* Create a Flex IO process.
	 * The flexio_app struct is passed to load the program.
	 * No process creation attributes are needed for this application (default outbox).
	 * Created SW struct will be returned through the given pointer.
	 */
	if (flexio_process_create(app_ctx.ibv_ctx, app_ctx.flexio_app, NULL, &app_fp)) {
		printf("Failed to create Flex IO process.\n");
		err = -1;
		goto cleanup;
	}

	/* Store the value of flexio_process in the structure for to pass it to the functions. */
	app_ctx.flexio_process = app_fp;

	/* Get the token for user debug access to the Flex IO process. */
	udbg_token = flexio_process_udbg_token_get(app_fp);

	/* If the token is 0, user debug access for the process is not allowed.
	 * If the token is not 0, the user can attach the FlexIO debugger to the process,
	 * set breakpoints, and debug the device application.
	 */
	if (udbg_token)
		printf("Use the token >>> %#lx <<< for debugging\n", udbg_token);

	/* Create a Flex IO message stream for process.
	 * Size of single message stream is MSG_HOST_BUFF_BSIZE.
	 * Working mode is synchronous.
	 * Level of debug in INFO.
	 * Transport mode - QP RC (possible alternatives - QP UC or QP UD)
	 * Output is stdout.
	 */
	stream_fattr.data_bsize = MSG_HOST_BUFF_BSIZE;
	stream_fattr.sync_mode = FLEXIO_MSG_DEV_SYNC_MODE_SYNC;
	stream_fattr.level = FLEXIO_MSG_DEV_INFO;
	stream_fattr.transport_mode = FLEXIO_MSG_TRANSPORT_QP_RC;

	if (flexio_msg_stream_create(app_fp, &stream_fattr, stdout, NULL,
				     &app_ctx.stream)) {
		printf("Failed to init device messaging environment, exiting App\n");
		err = -1;
		goto cleanup;
	}

	app_ctx.process_pd = flexio_process_get_pd(app_fp);
	app_ctx.process_uar = flexio_process_get_uar(app_fp);

	/* Create Window if RQ on Host is requested */
	if (app_ctx.rq_on_host) {
		if (flexio_window_create(app_fp, app_ctx.process_pd, &app_ctx.window)) {
			printf("Failed to create Flex IO Window\n");
			err = -1;
			goto cleanup;
		}
	}

	/* Create RX flow matcher. */


	// Calculate sport mask
	{
		uint32_t sport_mask_bits = 0;
		if (num_threads > 1) {
			sport_mask_bits = (uint32_t)ceil(log2((double)num_threads));
		}
		uint16_t sport_mask = (1 << sport_mask_bits) - 1;
		printf("Configured steering with Source Port Mask: 0x%x (%d bits) for %d threads\n", sport_mask, sport_mask_bits, num_threads);

		app_ctx.rx_matcher = FlowMatcher::create_rx(app_ctx.ibv_ctx, sport_mask);
		if (!app_ctx.rx_matcher) {
			printf("Failed to create RX matcher\n");
			goto cleanup;
		}
	}



	for (int i = 0; i < num_threads; i++) {
		auto thread = std::make_unique<ThreadResources>();
		thread->thread_id = i;

		/* Create an event handler. */
		if (create_app_event_handler(&app_ctx, thread.get())) {
			printf("Failed to create Flex IO event handler/thread %d.\n", i);
			err = -1;
			goto cleanup;
		}

		/* Create a Flex IO RQ to receive packets on the DPA. */
		if (create_app_rq(&app_ctx, thread.get())) {
			printf("Failed to create Flex RQ/thread %d.\n", i);
			err = -1;
			goto cleanup;
		}

		/* Create steering rules. */
		// We use the thread_id as the source port value to match against the mask.
		// So thread 0 handles packets where (sport & mask) == 0, etc.
		if (create_steering_rules(&app_ctx, thread.get(), udp_port, (uint16_t)i)) {
			printf("Failed to create Flex IO steering rules/thread %d.\n", i);
			err = -1;
			goto cleanup;
		}



		/* Copy the relevant information to DPA. */
		if (copy_app_data_to_dpa(&app_ctx, thread.get())) {
			printf("Failed to copy application data to DPA/thread %d.\n", i);
			err = -1;
			goto cleanup;
		}

		app_ctx.threads.push_back(std::move(thread));
	}

	printf("Ready to receive messages\n");

	/* Start event handlers */
	for (auto& thread : app_ctx.threads) {
		if (flexio_event_handler_run(thread->pp_eh, thread->app_data_daddr)) {
			printf("Failed to run event handler/thread %d.\n", thread->thread_id);
			err = -1;
			goto cleanup;
		}
	}

	/* Wait for Enter - the DPA sample is running in the meanwhile */
	// if (!fread(buf, 1, 1, stdin)) {
	// 	printf("Failed in fread\n");
	// }
	
	/* Polling loop for stats */
	while (1) {
		static uint64_t prev_bytes = 0;
		static uint64_t prev_packets = 0;
		static std::chrono::high_resolution_clock::time_point prev_time = std::chrono::high_resolution_clock::now();
		sleep(1);
		uint64_t total_packets = 0;
		uint64_t total_bytes = 0;
		
		// Call RPCs (use thread 0 context/process, though function is global, it runs on a thread?)
		// FlexIO RPC runs on a specific thread. We can run it on thread 0.
		// The function loops over all threads, so we just need one invocation.
		
		if (flexio_process_call(app_ctx.flexio_process, dpa_get_total_packets, &total_packets, (uint64_t)0) != FLEXIO_STATUS_SUCCESS) {
			printf("RPC call (packets) failed\n");
		}
		
		if (flexio_process_call(app_ctx.flexio_process, dpa_get_total_bytes, &total_bytes, (uint64_t)0) != FLEXIO_STATUS_SUCCESS) {
			printf("RPC call (bytes) failed\n");
		}
		std::chrono::high_resolution_clock::time_point current_time = std::chrono::high_resolution_clock::now();
		
		uint64_t diff_packets = total_packets - prev_packets;
		uint64_t diff_bytes = total_bytes - prev_bytes;
		std::chrono::duration<double> diff_time = current_time - prev_time;
		double mb_s = (double)diff_bytes / (1024.0 * 1024.0);
		prev_bytes = total_bytes;
		prev_packets = total_packets;
		prev_time = current_time;

		printf("Total Pkts/s: %.2f, BW/s: %.2f MB/s\n", diff_packets / diff_time.count(), mb_s / diff_time.count());
	}

cleanup:
	/* Clean up flow is done in reverse order of creation as there's a reference system
	 * that won't allow destroying resources that has references to existing resources.
	 */

	/* Clean up previously created rules */
	clean_up_rules(&app_ctx);

	/* Clean up app data daddr if created */
	for (auto& thread : app_ctx.threads) {
		if (thread->app_data_daddr &&
			flexio_buf_dev_free(app_fp, thread->app_data_daddr)) {
			printf("Failed to dealloc application data memory on Flex IO heap/thread %d\n", thread->thread_id);
			err = -1;
		}
		

		thread->flexio_rq.reset();

		if (thread->pp_eh && flexio_event_handler_destroy(thread->pp_eh)) {
			printf("Failed to destroy event handler/thread %d\n", thread->thread_id);
			err = -1;
		}
	}
	app_ctx.threads.clear();

	/* Clean up Window */
	if (app_ctx.window && flexio_window_destroy(app_ctx.window)) {
		printf("Failed to destroy Flex IO Window\n");
		err = -1;
	}

	if (app_ctx.stream && flexio_msg_stream_destroy(app_ctx.stream)) {
		printf("Failed to destroy conversion stream\n");
		err = -1;
	}

	/* Destroy the Flex IO process */
	if (flexio_process_destroy(app_fp)) {
		printf("Failed to destroy process.\n");
		err = -1;
	}

	/* Close the IBV device */
	if (ibv_close_device(app_ctx.ibv_ctx)) {
		printf("Failed to close ibv context.\n");
		err = -1;
	}

	return err;
}
