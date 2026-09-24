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

/* Source file with functions for Flow Steering Rules tables */

#include <malloc.h>
#include <stdint.h>
#include <assert.h>
#include <cstdio>
#include <cerrno>

#include "flow_steering_utils.h"

enum matcher_criteria {
	MATCHER_CRITERIA_EMPTY = 0,
	MATCHER_CRITERIA_OUTER = 1 << 0,
	MATCHER_CRITERIA_MISC  = 1 << 1,
	MATCHER_CRITERIA_INNER = 1 << 2,
	MATCHER_CRITERIA_MISC2 = 1 << 3,
	MATCHER_CRITERIA_MISC3 = 1 << 4,
};

struct mlx5_ifc_dr_match_spec_bits {
	uint8_t smac_47_16[0x20];

	uint8_t smac_15_0[0x10];
	uint8_t ethertype[0x10];

	uint8_t dmac_47_16[0x20];

	uint8_t dmac_15_0[0x10];
	uint8_t first_prio[0x3];
	uint8_t first_cfi[0x1];
	uint8_t first_vid[0xc];

	uint8_t ip_protocol[0x8];
	uint8_t ip_dscp[0x6];
	uint8_t ip_ecn[0x2];
	uint8_t cvlan_tag[0x1];
	uint8_t svlan_tag[0x1];
	uint8_t frag[0x1];
	uint8_t ip_version[0x4];
	uint8_t tcp_flags[0x9];

	uint8_t tcp_sport[0x10];
	uint8_t tcp_dport[0x10];

	uint8_t reserved_at_c0[0x18];
	uint8_t ip_ttl_hoplimit[0x8];

	uint8_t udp_sport[0x10];
	uint8_t udp_dport[0x10];

	uint8_t src_ip_127_96[0x20];

	uint8_t src_ip_95_64[0x20];

	uint8_t src_ip_63_32[0x20];

	uint8_t src_ip_31_0[0x20];

	uint8_t dst_ip_127_96[0x20];

	uint8_t dst_ip_95_64[0x20];

	uint8_t dst_ip_63_32[0x20];

	uint8_t dst_ip_31_0[0x20];
};

/* Every usage of this value is in bytes */
#define MATCH_VAL_BSIZE 64

/* Helper function to create matcher */
std::unique_ptr<FlowMatcher>
FlowMatcher::create_internal(struct ibv_context *ibv_ctx,
				 struct mlx5dv_flow_match_parameters *match_mask,
				 enum mlx5dv_dr_domain_type type,
				 bool create_sws)
{
	std::unique_ptr<FlowMatcher> flow_match(new FlowMatcher());

	flow_match->dr_domain = mlx5dv_dr_domain_create(ibv_ctx, type);
	if (!flow_match->dr_domain) {
		printf("Fail creating dr_domain (errno %d)\n", errno);
		return nullptr;
	}

	flow_match->dr_table_root = mlx5dv_dr_table_create(flow_match->dr_domain, 0);
	if (!flow_match->dr_table_root) {
		printf("Fail creating dr_table (errno %d)\n", errno);
		return nullptr;
	}

	flow_match->dr_matcher_root = mlx5dv_dr_matcher_create(flow_match->dr_table_root, 0,
							       MATCHER_CRITERIA_OUTER, match_mask);
	if (!flow_match->dr_matcher_root) {
		printf("Fail creating dr_matcher (errno %d)\n", errno);
		return nullptr;
	}

	if (create_sws) {
		flow_match->dr_table_sws = mlx5dv_dr_table_create(flow_match->dr_domain, 1);
		if (!flow_match->dr_table_sws) {
			printf("Fail creating dr_table_sws (errno %d)\n", errno);
			return nullptr;
		}

		flow_match->dr_matcher_sws = mlx5dv_dr_matcher_create(flow_match->dr_table_sws, 0,
								      MATCHER_CRITERIA_OUTER, match_mask);
		if (!flow_match->dr_matcher_sws) {
			printf("Fail creating dr_matcher_sws (errno %d)\n", errno);
			return nullptr;
		}
	}

	return flow_match;
}

std::unique_ptr<FlowMatcher> FlowMatcher::create_rx(struct ibv_context *ibv_ctx, uint16_t udp_sport_mask)
{
	struct mlx5dv_flow_match_parameters *match_mask;
	int match_mask_size;
	std::unique_ptr<FlowMatcher> matcher;

	/* mask & match value */
	match_mask_size = sizeof(*match_mask) + MATCH_VAL_BSIZE;
	match_mask = (struct mlx5dv_flow_match_parameters *)calloc(1, match_mask_size);
	assert(match_mask);

	match_mask->match_sz = MATCH_VAL_BSIZE;
	DEVX_SET(dr_match_spec, match_mask->match_buf, ethertype, 0xffff);
	DEVX_SET(dr_match_spec, match_mask->match_buf, ip_protocol, 0xff);
	DEVX_SET(dr_match_spec, match_mask->match_buf, udp_dport, 0xffff);
	DEVX_SET(dr_match_spec, match_mask->match_buf, udp_sport, udp_sport_mask);

	matcher = create_internal(ibv_ctx, match_mask,
						  MLX5DV_DR_DOMAIN_TYPE_NIC_RX, false);
	free(match_mask);

	return matcher;
}

std::unique_ptr<FlowMatcher> FlowMatcher::create_tx(struct ibv_context *ibv_ctx)
{
	struct mlx5dv_flow_match_parameters *match_mask;
	int match_mask_size;
	std::unique_ptr<FlowMatcher> matcher;

	/* mask & match value */
	match_mask_size = sizeof(*match_mask) + MATCH_VAL_BSIZE;
	match_mask = (struct mlx5dv_flow_match_parameters *)calloc(1, match_mask_size);
	assert(match_mask);

	match_mask->match_sz = MATCH_VAL_BSIZE;
	DEVX_SET(dr_match_spec, match_mask->match_buf, smac_47_16, 0xffffffff);
	DEVX_SET(dr_match_spec, match_mask->match_buf, smac_15_0, 0xffff);
	DEVX_SET(dr_match_spec, match_mask->match_buf, ethertype, 0xffff);
	
	matcher = create_internal(ibv_ctx, match_mask, MLX5DV_DR_DOMAIN_TYPE_FDB, true);
	free(match_mask);

	return matcher;
}

FlowMatcher::~FlowMatcher()
{
	int err;

	if (dr_matcher_sws) {
		err = mlx5dv_dr_matcher_destroy(dr_matcher_sws);
		if (err)
			printf("Failed to destroy dr_matcher_sws (errno %d)\n", err);
	}

	if (dr_table_sws) {
		err = mlx5dv_dr_table_destroy(dr_table_sws);
		if (err)
			printf("Failed to destroy dr_table_sws (errno %d)\n", err);
	}

	if (dr_matcher_root) {
		err = mlx5dv_dr_matcher_destroy(dr_matcher_root);
		if (err)
			printf("Failed to destroy dr_matcher_root (errno %d)\n", err);
	}

	if (dr_table_root) {
		err = mlx5dv_dr_table_destroy(dr_table_root);
		if (err)
			printf("Failed to destroy dr_table_root (errno %d)\n", err);
	}

	if (dr_domain) {
		err = mlx5dv_dr_domain_destroy(dr_domain);
		if (err)
			printf("Failed to destroy dr_domain (errno %d)\n", err);
	}
}

/* Helper to create rule */
std::unique_ptr<FlowRule> 
FlowRule::create_internal(struct mlx5dv_dr_matcher *matcher,
                         struct mlx5dv_dr_action *action,
                         struct mlx5dv_flow_match_parameters *match_value)
{
	struct mlx5dv_dr_action *actions[1];
	std::unique_ptr<FlowRule> flow_rule(new FlowRule());

	flow_rule->action = action;
	actions[0] = flow_rule->action;

	flow_rule->dr_rule = mlx5dv_dr_rule_create(matcher, match_value, 1, actions);
	if (!flow_rule->dr_rule) {
		printf("Fail creating dr_rule (errno %d).\n", errno);
		/* Action is destroyed by FlowRule destructor if unique_ptr is destroyed */
		return nullptr; 
	}

	return flow_rule;
}


std::unique_ptr<FlowRule> FlowRule::create_rx_udp_port_match(FlowMatcher *flow_match,
					  struct mlx5dv_devx_obj *tir_obj, uint16_t udp_dport, uint16_t udp_sport)
{
	struct mlx5dv_flow_match_parameters *match_value;
	int match_value_size;
	struct mlx5dv_dr_action *action;

	/* mask & match value */
	match_value_size = sizeof(*match_value) + MATCH_VAL_BSIZE;
	match_value = (struct mlx5dv_flow_match_parameters *)calloc(1, match_value_size);
	assert(match_value);

	match_value->match_sz = MATCH_VAL_BSIZE;
	DEVX_SET(dr_match_spec, match_value->match_buf, ethertype, 0x0800);
	DEVX_SET(dr_match_spec, match_value->match_buf, ip_protocol, 17);
	DEVX_SET(dr_match_spec, match_value->match_buf, udp_dport, udp_dport);
	DEVX_SET(dr_match_spec, match_value->match_buf, udp_sport, udp_sport);

	action = mlx5dv_dr_action_create_dest_devx_tir(tir_obj);
	if (!action) {
		printf("Failed creating TIR action (errno %d).\n", errno);
		free(match_value);
		return nullptr;
	}

	auto rule = create_internal(flow_match->dr_matcher_root, action, match_value);
	free(match_value);
	if (!rule) {
		/* If rule creation failed, action needs to be destroyed manually because rule pointer is null */
		mlx5dv_dr_action_destroy(action);
	}
	return rule;
}

std::unique_ptr<FlowRule> FlowRule::create_tx_fwd_to_vport_check_source_mac(FlowMatcher *flow_match, uint64_t smac)
{
	struct mlx5dv_flow_match_parameters *match_value;
	int match_value_size;
	struct mlx5dv_dr_action *action;

	/* mask & match value */
	match_value_size = sizeof(*match_value) + MATCH_VAL_BSIZE;
	match_value = (struct mlx5dv_flow_match_parameters *)calloc(1, match_value_size);
	assert(match_value);

	match_value->match_sz = MATCH_VAL_BSIZE;
	DEVX_SET(dr_match_spec, match_value->match_buf, smac_47_16, smac >> 16);
	DEVX_SET(dr_match_spec, match_value->match_buf, smac_15_0, smac % (1 << 16));
	DEVX_SET(dr_match_spec, match_value->match_buf, ethertype, 0x0800);

	action = mlx5dv_dr_action_create_dest_vport(flow_match->dr_domain, 0xFFFF);
	if (!action) {
		printf("Failed creating dest vport action (errno %d).\n", errno);
		free(match_value);
		return nullptr;
	}

	auto rule = create_internal(flow_match->dr_matcher_sws, action, match_value);
	free(match_value);
	if (!rule) {
		mlx5dv_dr_action_destroy(action);
	}
	return rule;
}

std::unique_ptr<FlowRule> FlowRule::create_tx_fwd_to_sws_table_check_source_mac(FlowMatcher *flow_match, uint64_t smac)
{
	struct mlx5dv_flow_match_parameters *match_value;
	int match_value_size;
	struct mlx5dv_dr_action *action;

	/* mask & match value */
	match_value_size = sizeof(*match_value) + MATCH_VAL_BSIZE;
	match_value = (struct mlx5dv_flow_match_parameters *)calloc(1, match_value_size);
	assert(match_value);

	match_value->match_sz = MATCH_VAL_BSIZE;
	DEVX_SET(dr_match_spec, match_value->match_buf, smac_47_16, smac >> 16);
	DEVX_SET(dr_match_spec, match_value->match_buf, smac_15_0, smac % (1 << 16));
	DEVX_SET(dr_match_spec, match_value->match_buf, ethertype, 0x0800);

	action = mlx5dv_dr_action_create_dest_table(flow_match->dr_table_sws);
	if (!action) {
		printf("Failed creating dest SWS table action (errno %d).\n", errno);
		free(match_value);
		return nullptr;
	}

	auto rule = create_internal(flow_match->dr_matcher_root, action, match_value);
	free(match_value);
	if (!rule) {
		mlx5dv_dr_action_destroy(action);
	}
	return rule;
}

FlowRule::~FlowRule()
{
	int err;
	if (dr_rule) {
		err = mlx5dv_dr_rule_destroy(dr_rule);
		if (err) 
			printf("Failed to destroy dr_rule (errno %d)\n", err);
	}
	if (action) {
		err = mlx5dv_dr_action_destroy(action);
		if (err)
			printf("Failed to destroy dr_action (errno %d)\n", err);
	}
}
