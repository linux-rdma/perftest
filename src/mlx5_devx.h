// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
// Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#ifndef MLX5_DEVX_H
#define MLX5_DEVX_H

enum mlx5_cap_mode {
	HCA_CAP_OPMOD_GET_MAX = 0,
	HCA_CAP_OPMOD_GET_CUR	= 1,
};

enum {
	MLX5_CMD_OP_QUERY_HCA_CAP = 0x100,
};

struct mlx5_ifc_cmd_hca_cap_bits {
	uint8_t         access_other_hca_roce[0x1];
	uint8_t         reserved_at_1[0x1e];
	uint8_t         vhca_resource_manager[0x1];

	uint8_t         hca_cap_2[0x1];
	uint8_t         reserved_at_21[0xf];
	uint8_t         vhca_id[0x10];

	uint8_t         reserved_at_40[0x20];

	uint8_t         reserved_at_60[0x2];
	uint8_t         qp_data_in_order[0x1];
	uint8_t         reserved_at_63[0x8];
	uint8_t         log_dma_mmo_max_size[0x5];
	uint8_t         reserved_at_70[0x10];

	uint8_t         log_max_srq_sz[0x8];
	uint8_t         log_max_qp_sz[0x8];
	uint8_t         reserved_at_90[0x3];
	uint8_t         isolate_vl_tc_new[0x1];
	uint8_t         reserved_at_94[0x4];
	uint8_t         prio_tag_required[0x1];
	uint8_t         reserved_at_99[0x2];
	uint8_t         log_max_qp[0x5];

	uint8_t         reserved_at_a0[0xb];
	uint8_t         log_max_srq[0x5];
	uint8_t         reserved_at_b0[0x10];

	uint8_t         reserved_at_c0[0x8];
	uint8_t         log_max_cq_sz[0x8];
	uint8_t         reserved_at_d0[0xb];
	uint8_t         log_max_cq[0x5];

	uint8_t         log_max_eq_sz[0x8];
	uint8_t         relaxed_ordering_write[0x1];
	uint8_t         reserved_at_e9[0x1];
	uint8_t         log_max_mkey[0x6];
	uint8_t         tunneled_atomic[0x1];
	uint8_t         as_notify[0x1];
	uint8_t         m_pci_port[0x1];
	uint8_t         m_vhca_mk[0x1];
	uint8_t         cmd_on_behalf[0x1];
	uint8_t         device_emulation_manager[0x1];
	uint8_t         terminate_scatter_list_mkey[0x1];
	uint8_t         repeated_mkey[0x1];
	uint8_t         dump_fill_mkey[0x1];
	uint8_t         reserved_at_f9[0x3];
	uint8_t         log_max_eq[0x4];

	uint8_t         max_indirection[0x8];
	uint8_t         fixed_buffer_size[0x1];
	uint8_t         log_max_mrw_sz[0x7];
	uint8_t         force_teardown[0x1];
	uint8_t         fast_teardown[0x1];
	uint8_t         log_max_bsf_list_size[0x6];
	uint8_t         umr_extended_translation_offset[0x1];
	uint8_t         null_mkey[0x1];
	uint8_t         log_max_klm_list_size[0x6];

	uint8_t         reserved_at_120[0x2];
	uint8_t         qpc_extension[0x1];
	uint8_t         reserved_at_123[0x7];
	uint8_t         log_max_ra_req_dc[0x6];
	uint8_t         reserved_at_130[0xa];
	uint8_t         log_max_ra_res_dc[0x6];

	uint8_t         reserved_at_140[0x7];
	uint8_t         sig_crc64_xp10[0x1];
	uint8_t         sig_crc32c[0x1];
	uint8_t         reserved_at_149[0x1];
	uint8_t         log_max_ra_req_qp[0x6];
	uint8_t         reserved_at_150[0x1];
	uint8_t         rts2rts_qp_udp_sport[0x1];
	uint8_t         rts2rts_lag_tx_port_affinity[0x1];
	uint8_t         dma_mmo_sq[0x1];
	uint8_t         reserved_at_154[0x6];
	uint8_t         log_max_ra_res_qp[0x6];

	uint8_t         end_pad[0x1];
	uint8_t         cc_query_allowed[0x1];
	uint8_t         cc_modify_allowed[0x1];
	uint8_t         start_pad[0x1];
	uint8_t         cache_line_128byte[0x1];
	uint8_t         gid_table_size_ro[0x1];
	uint8_t         pkey_table_size_ro[0x1];
	uint8_t         reserved_at_167[0x1];
	uint8_t         rnr_nak_q_counters[0x1];
	uint8_t         rts2rts_qp_counters_set_id[0x1];
	uint8_t         rts2rts_qp_dscp[0x1];
	uint8_t         reserved_at_16b[0x4];
	uint8_t         qcam_reg[0x1];
	uint8_t         gid_table_size[0x10];

	uint8_t         out_of_seq_cnt[0x1];
	uint8_t         vport_counters[0x1];
	uint8_t         retransmission_q_counters[0x1];
	uint8_t         debug[0x1];
	uint8_t         modify_rq_counters_set_id[0x1];
	uint8_t         rq_delay_drop[0x1];
	uint8_t         max_qp_cnt[0xa];
	uint8_t         pkey_table_size[0x10];

	uint8_t         vport_group_manager[0x1];
	uint8_t         vhca_group_manager[0x1];
	uint8_t         ib_virt[0x1];
	uint8_t         eth_virt[0x1];
	uint8_t         vnic_env_queue_counters[0x1];
	uint8_t         ets[0x1];
	uint8_t         nic_flow_table[0x1];
	uint8_t         eswitch_manager[0x1];
	uint8_t         device_memory[0x1];
	uint8_t         mcam_reg[0x1];
	uint8_t         pcam_reg[0x1];
	uint8_t         local_ca_ack_delay[0x5];
	uint8_t         port_module_event[0x1];
	uint8_t         enhanced_retransmission_q_counters[0x1];
	uint8_t         port_checks[0x1];
	uint8_t         pulse_gen_control[0x1];
	uint8_t         disable_link_up_by_init_hca[0x1];
	uint8_t         beacon_led[0x1];
	uint8_t         port_type[0x2];
	uint8_t         num_ports[0x8];

	uint8_t         reserved_at_1c0[0x1];
	uint8_t         pps[0x1];
	uint8_t         pps_modify[0x1];
	uint8_t         log_max_msg[0x5];
	uint8_t         multi_path_xrc_rdma[0x1];
	uint8_t         multi_path_dc_rdma[0x1];
	uint8_t         multi_path_rc_rdma[0x1];
	uint8_t         traffic_fast_control[0x1];
	uint8_t         max_tc[0x4];
	uint8_t         temp_warn_event[0x1];
	uint8_t         dcbx[0x1];
	uint8_t         general_notification_event[0x1];
	uint8_t         multi_prio_sq[0x1];
	uint8_t         afu_owner[0x1];
	uint8_t         fpga[0x1];
	uint8_t         rol_s[0x1];
	uint8_t         rol_g[0x1];
	uint8_t         ib_port_sniffer[0x1];
	uint8_t         wol_s[0x1];
	uint8_t         wol_g[0x1];
	uint8_t         wol_a[0x1];
	uint8_t         wol_b[0x1];
	uint8_t         wol_m[0x1];
	uint8_t         wol_u[0x1];
	uint8_t         wol_p[0x1];

	uint8_t         stat_rate_support[0x10];
	uint8_t         sig_block_4048[0x1];
	uint8_t         reserved_at_1f1[0xb];
	uint8_t         cqe_version[0x4];

	uint8_t         compact_address_vector[0x1];
	uint8_t         eth_striding_wq[0x1];
	uint8_t         reserved_at_202[0x1];
	uint8_t         ipoib_enhanced_offloads[0x1];
	uint8_t         ipoib_basic_offloads[0x1];
	uint8_t         ib_striding_wq[0x1];
	uint8_t         repeated_block_disabled[0x1];
	uint8_t         umr_modify_entity_size_disabled[0x1];
	uint8_t         umr_modify_atomic_disabled[0x1];
	uint8_t         umr_indirect_mkey_disabled[0x1];
	uint8_t         umr_fence[0x2];
	uint8_t         dc_req_sctr_data_cqe[0x1];
	uint8_t         dc_connect_qp[0x1];
	uint8_t         dc_cnak_trace[0x1];
	uint8_t         drain_sigerr[0x1];
	uint8_t         cmdif_checksum[0x2];
	uint8_t         sigerr_cqe[0x1];
	uint8_t         reserved_at_213[0x1];
	uint8_t         wq_signature[0x1];
	uint8_t         sctr_data_cqe[0x1];
	uint8_t         reserved_at_216[0x1];
	uint8_t         sho[0x1];
	uint8_t         tph[0x1];
	uint8_t         rf[0x1];
	uint8_t         dct[0x1];
	uint8_t         qos[0x1];
	uint8_t         eth_net_offloads[0x1];
	uint8_t         roce[0x1];
	uint8_t         atomic[0x1];
	uint8_t         extended_retry_count[0x1];

	uint8_t         cq_oi[0x1];
	uint8_t         cq_resize[0x1];
	uint8_t         cq_moderation[0x1];
	uint8_t         cq_period_mode_modify[0x1];
	uint8_t         cq_invalidate[0x1];
	uint8_t         reserved_at_225[0x1];
	uint8_t         cq_eq_remap[0x1];
	uint8_t         pg[0x1];
	uint8_t         block_lb_mc[0x1];
	uint8_t         exponential_backoff[0x1];
	uint8_t         scqe_break_moderation[0x1];
	uint8_t         cq_period_start_from_cqe[0x1];
	uint8_t         cd[0x1];
	uint8_t         atm[0x1];
	uint8_t         apm[0x1];
	uint8_t         vector_calc[0x1];
	uint8_t         umr_ptr_rlkey[0x1];
	uint8_t         imaicl[0x1];
	uint8_t         qp_packet_based[0x1];
	uint8_t         reserved_at_233[0x1];
	uint8_t         ipoib_enhanced_pkey_change[0x1];
	uint8_t         initiator_src_dct_in_cqe[0x1];
	uint8_t         qkv[0x1];
	uint8_t         pkv[0x1];
	uint8_t         set_deth_sqpn[0x1];
	uint8_t         rts2rts_primary_sl[0x1];
	uint8_t         initiator_src_dct[0x1];
	uint8_t         dc_v2[0x1];
	uint8_t         xrc[0x1];
	uint8_t         ud[0x1];
	uint8_t         uc[0x1];
	uint8_t         rc[0x1];

	uint8_t         uar_4k[0x1];
	uint8_t         reserved_at_241[0x9];
	uint8_t         uar_sz[0x6];
	uint8_t         reserved_at_250[0x2];
	uint8_t         umem_uid_0[0x1];
	uint8_t         log_max_dc_cnak_qps[0x5];
	uint8_t         log_pg_sz[0x8];

	uint8_t         bf[0x1];
	uint8_t         driver_version[0x1];
	uint8_t         pad_tx_eth_packet[0x1];
	uint8_t         query_driver_version[0x1];
	uint8_t         max_qp_retry_freq[0x1];
	uint8_t         qp_by_name[0x1];
	uint8_t         mkey_by_name[0x1];
	uint8_t         reserved_at_267[0x1];
	uint8_t         suspend_qp_uc[0x1];
	uint8_t         suspend_qp_ud[0x1];
	uint8_t         suspend_qp_rc[0x1];
	uint8_t         log_bf_reg_size[0x5];
	uint8_t         reserved_at_270[0x6];
	uint8_t         lag_dct[0x2];
	uint8_t         lag_tx_port_affinity[0x1];
	uint8_t         reserved_at_279[0x2];
	uint8_t         lag_master[0x1];
	uint8_t         num_lag_ports[0x4];

	uint8_t         num_of_diagnostic_counters[0x10];
	uint8_t         max_wqe_sz_sq[0x10];

	uint8_t         reserved_at_2a0[0x10];
	uint8_t         max_wqe_sz_rq[0x10];

	uint8_t         max_flow_counter_31_16[0x10];
	uint8_t         max_wqe_sz_sq_dc[0x10];

	uint8_t         reserved_at_2e0[0x7];
	uint8_t         max_qp_mcg[0x19];

	uint8_t         mlnx_tag_ethertype[0x10];
	uint8_t         reserved_at_310[0x8];
	uint8_t         log_max_mcg[0x8];

	uint8_t         reserved_at_320[0x3];
	uint8_t         log_max_transport_domain[0x5];
	uint8_t         reserved_at_328[0x3];
	uint8_t         log_max_pd[0x5];
	uint8_t         reserved_at_330[0xb];
	uint8_t         log_max_xrcd[0x5];

	uint8_t         nic_receive_steering_discard[0x1];
	uint8_t         receive_discard_vport_down[0x1];
	uint8_t         transmit_discard_vport_down[0x1];
	uint8_t         eq_overrun_count[0x1];
	uint8_t         nic_receive_steering_depth[0x1];
	uint8_t         invalid_command_count[0x1];
	uint8_t         quota_exceeded_count[0x1];
	uint8_t         reserved_at_347[0x1];
	uint8_t         log_max_flow_counter_bulk[0x8];
	uint8_t         max_flow_counter_15_0[0x10];

	uint8_t         modify_tis[0x1];
	uint8_t         reserved_at_361[0x2];
	uint8_t         log_max_rq[0x5];
	uint8_t         reserved_at_368[0x3];
	uint8_t         log_max_sq[0x5];
	uint8_t         reserved_at_370[0x3];
	uint8_t         log_max_tir[0x5];
	uint8_t         reserved_at_378[0x3];
	uint8_t         log_max_tis[0x5];

	uint8_t         basic_cyclic_rcv_wqe[0x1];
	uint8_t         reserved_at_381[0x2];
	uint8_t         log_max_rmp[0x5];
	uint8_t         reserved_at_388[0x3];
	uint8_t         log_max_rqt[0x5];
	uint8_t         reserved_at_390[0x3];
	uint8_t         log_max_rqt_size[0x5];
	uint8_t         reserved_at_398[0x3];
	uint8_t         log_max_tis_per_sq[0x5];

	uint8_t         ext_stride_num_range[0x1];
	uint8_t         reserved_at_3a1[0x2];
	uint8_t         log_max_stride_sz_rq[0x5];
	uint8_t         reserved_at_3a8[0x3];
	uint8_t         log_min_stride_sz_rq[0x5];
	uint8_t         reserved_at_3b0[0x3];
	uint8_t         log_max_stride_sz_sq[0x5];
	uint8_t         reserved_at_3b8[0x3];
	uint8_t         log_min_stride_sz_sq[0x5];

	uint8_t         hairpin[0x1];
	uint8_t         reserved_at_3c1[0x2];
	uint8_t         log_max_hairpin_queues[0x5];
	uint8_t         reserved_at_3c8[0x3];
	uint8_t         log_max_hairpin_wq_data_sz[0x5];
	uint8_t         reserved_at_3d0[0x3];
	uint8_t         log_max_hairpin_num_packets[0x5];
	uint8_t         reserved_at_3d8[0x3];
	uint8_t         log_max_wq_sz[0x5];

	uint8_t         nic_vport_change_event[0x1];
	uint8_t         disable_local_lb_uc[0x1];
	uint8_t         disable_local_lb_mc[0x1];
	uint8_t         log_min_hairpin_wq_data_sz[0x5];
	uint8_t         reserved_at_3e8[0x3];
	uint8_t         log_max_vlan_list[0x5];
	uint8_t         reserved_at_3f0[0x1];
	uint8_t         aes_xts_single_block_le_tweak[0x1];
	uint8_t         reserved_at_3f2[0x1];
	uint8_t         log_max_current_mc_list[0x5];
	uint8_t         reserved_at_3f8[0x3];
	uint8_t         log_max_current_uc_list[0x5];

	uint8_t         general_obj_types[0x40];

	uint8_t         reserved_at_440[0x4];
	uint8_t         steering_format_version[0x4];
	uint8_t         create_qp_start_hint[0x18];

	uint8_t         reserved_at_460[0x9];
	uint8_t         crypto[0x1];
	uint8_t         reserved_at_46a[0x6];
	uint8_t         max_num_eqs[0x10];

	uint8_t         sigerr_domain_and_sig_type[0x1];
	uint8_t         reserved_at_481[0x2];
	uint8_t         log_max_l2_table[0x5];
	uint8_t         reserved_at_488[0x8];
	uint8_t         log_uar_page_sz[0x10];

	uint8_t         reserved_at_4a0[0x20];

	uint8_t         device_frequency_mhz[0x20];

	uint8_t         device_frequency_khz[0x20];

	uint8_t         capi[0x1];
	uint8_t         create_pec[0x1];
	uint8_t         nvmf_target_offload[0x1];
	uint8_t         capi_invalidate[0x1];
	uint8_t         reserved_at_504[0x17];
	uint8_t         log_max_pasid[0x5];

	uint8_t         num_of_uars_per_page[0x20];

	uint8_t         flex_parser_protocols[0x20];

	uint8_t         reserved_at_560[0x10];
	uint8_t         flex_parser_header_modify[0x1];
	uint8_t         reserved_at_571[0x2];
	uint8_t         log_max_guaranteed_connections[0x5];
	uint8_t         reserved_at_578[0x3];
	uint8_t         log_max_dct_connections[0x5];

	uint8_t         log_max_atomic_size_qp[0x8];
	uint8_t         reserved_at_588[0x10];
	uint8_t         log_max_atomic_size_dc[0x8];

	uint8_t         reserved_at_5a0[0x1c];
	uint8_t         mini_cqe_resp_stride_index[0x1];
	uint8_t         cqe_128_always[0x1];
	uint8_t         cqe_compression_128b[0x1];
	uint8_t         cqe_compression[0x1];

	uint8_t         cqe_compression_timeout[0x10];
	uint8_t         cqe_compression_max_num[0x10];

	uint8_t         reserved_at_5e0[0x8];
	uint8_t         flex_parser_id_gtpu_dw_0[0x4];
	uint8_t         log_max_tm_offloaded_op_size[0x4];
	uint8_t         tag_matching[0x1];
	uint8_t         rndv_offload_rc[0x1];
	uint8_t         rndv_offload_dc[0x1];
	uint8_t         log_tag_matching_list_sz[0x5];
	uint8_t         reserved_at_5f8[0x3];
	uint8_t         log_max_xrq[0x5];

	uint8_t         affiliate_nic_vport_criteria[0x8];
	uint8_t         native_port_num[0x8];
	uint8_t         num_vhca_ports[0x8];
	uint8_t         flex_parser_id_gtpu_teid[0x4];
	uint8_t         reserved_at_61c[0x1];
	uint8_t         trusted_vnic_vhca[0x1];
	uint8_t         sw_owner_id[0x1];
	uint8_t         reserve_not_to_use[0x1];
	uint8_t         reserved_at_620[0x60];
	uint8_t         sf[0x1];
	uint8_t         reserved_at_682[0x43];
	uint8_t         flex_parser_id_geneve_opt_0[0x4];
	uint8_t         flex_parser_id_icmp_dw1[0x4];
	uint8_t         flex_parser_id_icmp_dw0[0x4];
	uint8_t         flex_parser_id_icmpv6_dw1[0x4];
	uint8_t         flex_parser_id_icmpv6_dw0[0x4];
	uint8_t         flex_parser_id_outer_first_mpls_over_gre[0x4];
	uint8_t         flex_parser_id_outer_first_mpls_over_udp_label[0x4];

	uint8_t         reserved_at_6e0[0x20];

	uint8_t         flex_parser_id_gtpu_dw_2[0x4];
	uint8_t         flex_parser_id_gtpu_first_ext_dw_0[0x4];
	uint8_t         reserved_at_708[0x18];

	uint8_t         reserved_at_720[0x20];

	uint8_t         reserved_at_740[0x8];
	uint8_t         dma_mmo_qp[0x1];
	uint8_t         reserved_at_749[0x17];

	uint8_t         reserved_at_760[0x3];
	uint8_t         log_max_num_header_modify_argument[0x5];
	uint8_t         reserved_at_768[0x4];
	uint8_t         log_header_modify_argument_granularity[0x4];
	uint8_t         reserved_at_770[0x3];
	uint8_t         log_header_modify_argument_max_alloc[0x5];
	uint8_t         reserved_at_778[0x8];

	uint8_t         reserved_at_780[0x40];

	uint8_t         match_definer_format_supported[0x40];
};

struct mlx5_ifc_query_hca_cap_in_bits {
	uint8_t         opcode[0x10];
	uint8_t         reserved_at_10[0x10];

	uint8_t         reserved_at_20[0x10];
	uint8_t         op_mod[0x10];

	uint8_t         other_function[0x1];
	uint8_t         reserved_at_41[0xf];
	uint8_t         function_id[0x10];

	uint8_t         reserved_at_60[0x20];
};

struct mlx5_ifc_query_hca_cap_out_bits {
	uint8_t         status[0x8];
	uint8_t         reserved_at_8[0x18];

	uint8_t         syndrome[0x20];

	uint8_t         reserved_at_40[0x40];

	struct mlx5_ifc_cmd_hca_cap_bits cmd_hca_cap;
};

#endif /* MLX5_DEVX_H */
