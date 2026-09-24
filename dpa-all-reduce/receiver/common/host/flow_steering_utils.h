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

/* Header file with declaration of structures and functions for
 * Flow Steering Rules tables */

#ifndef __COM_FLOW_HOST_H__
#define __COM_FLOW_HOST_H__

#include <infiniband/mlx5dv.h>
#include <memory> /* For std::unique_ptr */

/* Forward declarations */
class FlowRule;

/* The cumulative structure of the flow matcher */
class FlowMatcher {
public:
	/* Create a flow matcher for Ethernet packets received on the NIC.
	 *  ibv_ctx - context of the IBV device.
	 */
	/* Create a flow matcher for Ethernet packets received on the NIC.
	 *  ibv_ctx - context of the IBV device.
	 *  udp_sport_mask - Mask for UDP source port matching
	 */
	static std::unique_ptr<FlowMatcher> create_rx(struct ibv_context *ibv_ctx, uint16_t udp_sport_mask);

	/* Create a flow matcher for Ethernet packets transmitted on the NIC.
	 *  ibv_ctx - context of the IBV device.
	 */
	static std::unique_ptr<FlowMatcher> create_tx(struct ibv_context *ibv_ctx);

	~FlowMatcher();

	/* Delete copy constructor and assignment operator */
	FlowMatcher(const FlowMatcher&) = delete;
	FlowMatcher& operator=(const FlowMatcher&) = delete;

	/* Allow move constructor and assignment operator */
	FlowMatcher(FlowMatcher&&) = default;
	FlowMatcher& operator=(FlowMatcher&&) = default;

private:
	/* Private constructor to force use of factory methods */
	FlowMatcher() = default;

	struct mlx5dv_dr_domain *dr_domain = nullptr;
	struct mlx5dv_dr_table *dr_table_root = nullptr;
	struct mlx5dv_dr_matcher *dr_matcher_root = nullptr;
	struct mlx5dv_dr_table *dr_table_sws = nullptr;
	struct mlx5dv_dr_matcher *dr_matcher_sws = nullptr;

	static std::unique_ptr<FlowMatcher>
	create_internal(struct ibv_context *ibv_ctx,
			struct mlx5dv_flow_match_parameters *match_mask,
			enum mlx5dv_dr_domain_type type,
			bool create_sws);

	friend class FlowRule;
};

/* The cumulative structure of the flow rule */
class FlowRule {
public:
	/* Create a SW flow steering rule for ethernet packets received on the NIC.
	 *  ibv_ctx - context of the IBV device.
	 *  tir_obj - TIR mlx5dv object
	 */
	/* Create a SW flow steering rule for ethernet packets received on the NIC.
	 *  ibv_ctx - context of the IBV device.
	 *  tir_obj - TIR mlx5dv object
	 *  udp_dport - Destination UDP port to match
     *  udp_sport - Source UDP port to match
	 */
	static std::unique_ptr<FlowRule> create_rx_udp_port_match(FlowMatcher *flow_match,
							struct mlx5dv_devx_obj *tir_obj, uint16_t udp_dport, uint16_t udp_sport);

	/* Create a flow rule for Ethernet packets transmitted on the NIC.
	 *  flow_match - pointer to the previously created flow_matcher structure.
	 *  smac - Source MAC address
	 */
	static std::unique_ptr<FlowRule> create_tx_fwd_to_vport_check_source_mac(FlowMatcher *flow_match, uint64_t smac);

	/* Create a flow rule for Ethernet packets transmitted on the NIC through
	 *  thw software-steering table.
	 *  flow_match - pointer to the previously created flow_matcher structure.
	 *  smac - Source MAC address
	 */
	static std::unique_ptr<FlowRule> create_tx_fwd_to_sws_table_check_source_mac(FlowMatcher *flow_match, uint64_t smac);

	~FlowRule();

	/* Delete copy constructor and assignment operator */
	FlowRule(const FlowRule&) = delete;
	FlowRule& operator=(const FlowRule&) = delete;

	/* Allow move constructor and assignment operator */
	FlowRule(FlowRule&&) = default;
	FlowRule& operator=(FlowRule&&) = default;

private:
	/* Private constructor to force use of factory methods */
	FlowRule() = default;

	static std::unique_ptr<FlowRule> 
	create_internal(struct mlx5dv_dr_matcher *matcher,
			struct mlx5dv_dr_action *action,
			struct mlx5dv_flow_match_parameters *match_value);

	struct mlx5dv_dr_action *action = nullptr;
	struct mlx5dv_dr_rule *dr_rule = nullptr;
};

#endif
