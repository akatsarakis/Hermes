//
// Created by akatsarakis on 23/05/18.
//

#ifndef HERMES_INLINE_UTIL_H
#define HERMES_INLINE_UTIL_H

#include <infiniband/verbs.h>
#include <optik_mod.h>
#include "spacetime.h"
#include "config.h"
#include "util.h"


static inline void
poll_cq_for_credits(struct ibv_cq *credit_recv_cq, struct ibv_wc *credit_wc,
					struct ibv_recv_wr* credit_recv_wr, struct hrd_ctrl_blk* cb,
					uint8_t credits[][MACHINE_NUM], uint16_t worker_lid);

static inline uint8_t
node_is_in_membership(spacetime_group_membership last_group_membership, int node_id);
/* ---------------------------------------------------------------------------
------------------------------------GENERIC-----------------------------------
---------------------------------------------------------------------------*/
static inline void
post_receives(struct hrd_ctrl_blk *cb, uint16_t num_of_receives,
			  uint8_t buff_type, void *recv_buff, int *push_ptr,
			  struct ibv_recv_wr *recv_wr)
{
	int i, ret, qp_id, req_size, recv_q_depth, max_recv_wrs;
	struct ibv_recv_wr *bad_recv_wr;
	void* next_buff_addr;

	switch(buff_type){
		case ST_INV_BUFF:
			req_size = INV_RECV_REQ_SIZE;
			recv_q_depth = RECV_INV_Q_DEPTH;
			max_recv_wrs = MAX_RECV_INV_WRS;
			qp_id = INV_UD_QP_ID;
			break;
		case ST_ACK_BUFF:
			req_size = ACK_RECV_REQ_SIZE;
			recv_q_depth = RECV_ACK_Q_DEPTH;
			max_recv_wrs = MAX_RECV_ACK_WRS;
			qp_id = ACK_UD_QP_ID;
			break;
		case ST_VAL_BUFF:
			req_size = VAL_RECV_REQ_SIZE;
			recv_q_depth = RECV_VAL_Q_DEPTH;
			max_recv_wrs = MAX_RECV_VAL_WRS;
			qp_id = VAL_UD_QP_ID;
			break;
		default: assert(0);
	}
	if(ENABLE_ASSERTIONS)
		assert(num_of_receives <= max_recv_wrs);

	for(i = 0; i < num_of_receives; i++) {
		next_buff_addr = ((uint8_t*) recv_buff) + (*push_ptr * req_size);
		memset(next_buff_addr, 0, (size_t) req_size); //reset the buffer before posting the receive
		if(BATCH_POST_RECVS_TO_NIC)
			recv_wr[i].sg_list->addr = (uintptr_t) next_buff_addr;
		else
			hrd_post_dgram_recv(cb->dgram_qp[qp_id], next_buff_addr,
								req_size, cb->dgram_buf_mr->lkey);
		HRD_MOD_ADD(*push_ptr, recv_q_depth);
	}

	if(BATCH_POST_RECVS_TO_NIC) {
		recv_wr[num_of_receives - 1].next = NULL;
		if (ENABLE_ASSERTIONS) {
			for (i = 0; i < num_of_receives - 1; i++) {
				assert(recv_wr[i].num_sge == 1);
				assert(recv_wr[i].next == &recv_wr[i + 1]);
				assert(recv_wr[i].sg_list->length == req_size);
				assert(recv_wr[i].sg_list->lkey == cb->dgram_buf_mr->lkey);
			}
			assert(recv_wr[i].next == NULL);
		}
		ret = ibv_post_recv(cb->dgram_qp[qp_id], &recv_wr[0], &bad_recv_wr);
		CPE(ret, "ibv_post_recv error: posting recvs for credits before val bcast", ret);
		//recover next ptr of last wr to NULL
		recv_wr[num_of_receives - 1].next = (max_recv_wrs == num_of_receives - 1) ?
											NULL : &recv_wr[num_of_receives];
	}
}

static inline void
poll_buff_and_post_recvs(void *incoming_buff, uint8_t buff_type, int *buf_pull_ptr,
						 void *recv_ops, int *ops_push_ptr,
						 struct ibv_cq *completion_q, struct ibv_wc *work_completions,
						 struct hrd_ctrl_blk *cb, int *recv_push_ptr,
						 struct ibv_recv_wr *recv_wr,
						 uint8_t credits[][MACHINE_NUM], uint16_t worker_lid)
{
	void* next_packet_reqs, *recv_op_ptr, *next_req;
	uint8_t *next_packet_req_num_ptr;
	int index = 0, recv_q_depth = 0, max_credits = 0, i = 0, j = 0,
			packets_polled = 0, reqs_polled = 0;
	uint8_t qp_credits_to_inc = 0, sender = 0;
	size_t req_size = 0;
    if(ENABLE_ASSERTIONS){
		if(*ops_push_ptr != 0)
			printf("Poll buff: %s, reqs_polled: %d, *ops_push: %d\n",
				   code_to_str(buff_type), reqs_polled, *ops_push_ptr);
		assert(*ops_push_ptr == 0);
	}
	switch(buff_type){
		case ST_INV_BUFF:
			qp_credits_to_inc = ACK_UD_QP_ID;
			recv_q_depth = RECV_INV_Q_DEPTH;
			req_size = sizeof(spacetime_inv_t);
			if(ENABLE_ASSERTIONS)
				max_credits = ACK_CREDITS;
			break;
		case ST_ACK_BUFF:
			qp_credits_to_inc = INV_UD_QP_ID;
			recv_q_depth = RECV_ACK_Q_DEPTH;
			req_size = sizeof(spacetime_ack_t);
			if(ENABLE_ASSERTIONS)
				max_credits = INV_CREDITS;
			break;
		case ST_VAL_BUFF:
			qp_credits_to_inc = CRD_UD_QP_ID;
			recv_q_depth = RECV_VAL_Q_DEPTH;
			req_size = sizeof(spacetime_val_t);
			if(ENABLE_ASSERTIONS)
				max_credits = CRD_CREDITS;
			break;
		default: assert(0);
	}

	//poll completion q
	packets_polled = ibv_poll_cq(completion_q, MAX_BATCH_OPS_SIZE - *ops_push_ptr, work_completions);

	for(i = 0; i < packets_polled; i++){
		index = (*buf_pull_ptr + 1) % recv_q_depth;
		switch (buff_type) {
			case ST_INV_BUFF:
				sender = ((ud_req_inv_t *) incoming_buff)[index].packet.reqs[0].sender;
				next_packet_reqs = &((ud_req_inv_t *) incoming_buff)[index].packet.reqs;
				next_packet_req_num_ptr = &((ud_req_inv_t *) incoming_buff)[index].packet.req_num;
				if(ENABLE_ASSERTIONS){
					assert(*next_packet_req_num_ptr > 0 && *next_packet_req_num_ptr <= INV_MAX_REQ_COALESCE);
					for(j = 0; j < *next_packet_req_num_ptr; j++){
						assert(((spacetime_inv_t*) next_packet_reqs)[j].version % 2 == 0);
						assert(((spacetime_inv_t*) next_packet_reqs)[j].opcode == ST_OP_INV);
						assert(((spacetime_inv_t*) next_packet_reqs)[j].val_len == ST_VALUE_SIZE);
						assert(REMOTE_MACHINES != 1 || ((spacetime_inv_t*) next_packet_reqs)[j].tie_breaker_id == REMOTE_MACHINES - machine_id);
						assert(REMOTE_MACHINES != 1 || ((spacetime_inv_t*) next_packet_reqs)[j].sender == REMOTE_MACHINES - machine_id);
					}
				}
				break;
			case ST_ACK_BUFF:
				sender = ((ud_req_ack_t *) incoming_buff)[index].packet.reqs[0].sender;
				next_packet_reqs = &((ud_req_ack_t *) incoming_buff)[index].packet.reqs;
				next_packet_req_num_ptr = &((ud_req_ack_t *) incoming_buff)[index].packet.req_num;
				if(ENABLE_ASSERTIONS){
					assert(*next_packet_req_num_ptr > 0 && *next_packet_req_num_ptr <= ACK_MAX_REQ_COALESCE);
					for(j = 0; j < *next_packet_req_num_ptr; j++){
						assert(((spacetime_ack_t*) next_packet_reqs)[j].version % 2 == 0);
						assert(((spacetime_ack_t*) next_packet_reqs)[j].opcode == ST_OP_ACK);
						assert(REMOTE_MACHINES != 1 || ((spacetime_ack_t*) next_packet_reqs)[j].sender == REMOTE_MACHINES - machine_id);
						assert(group_membership.num_of_alive_remotes != REMOTE_MACHINES ||
							   ((spacetime_ack_t*) next_packet_reqs)[j].tie_breaker_id == machine_id);
					}
				}
				break;
			case ST_VAL_BUFF:
				sender = ((ud_req_val_t *) incoming_buff)[index].packet.reqs[0].sender;
				next_packet_reqs = &((ud_req_val_t *) incoming_buff)[index].packet.reqs;
				next_packet_req_num_ptr = &((ud_req_val_t *) incoming_buff)[index].packet.req_num;
				if(ENABLE_ASSERTIONS){
					assert(*next_packet_req_num_ptr > 0 && *next_packet_req_num_ptr <= VAL_MAX_REQ_COALESCE);
					for(j = 0; j < *next_packet_req_num_ptr; j++){
						assert(((spacetime_val_t*) next_packet_reqs)[j].version % 2 == 0);
						assert(((spacetime_val_t*) next_packet_reqs)[j].opcode == ST_OP_VAL);
						assert(REMOTE_MACHINES != 1 || ((spacetime_val_t*) next_packet_reqs)[j].tie_breaker_id == REMOTE_MACHINES - machine_id);
						assert(REMOTE_MACHINES != 1 || ((spacetime_val_t*) next_packet_reqs)[j].sender == REMOTE_MACHINES - machine_id);
					}
				}
				break;
			default:
				assert(0);
		}
		for(j = 0; j < *next_packet_req_num_ptr; j++){
			recv_op_ptr = ((uint8_t *) recv_ops) + (*ops_push_ptr * req_size);
			next_req = &((uint8_t*) next_packet_reqs)[j * req_size];
			memcpy(recv_op_ptr, next_req, req_size);
			reqs_polled++;
			(*ops_push_ptr)++;
			credits[qp_credits_to_inc][sender]++; //increment packet credits
		}

		*next_packet_req_num_ptr = 0; //TODO can be removed since we already reset on posting receives
		HRD_MOD_ADD(*buf_pull_ptr, recv_q_depth);

		if(ENABLE_ASSERTIONS){
			assert(credits[qp_credits_to_inc][sender] <= max_credits);
            if(reqs_polled != *ops_push_ptr)
				printf("Poll buff: %s, reqs_polled: %d, *ops_push: %d\n",
					   code_to_str(buff_type), reqs_polled, *ops_push_ptr);
			assert(reqs_polled == *ops_push_ptr);
		}
	}

	//Refill recvs
	if(packets_polled > 0)
		post_receives(cb, (uint16_t) packets_polled, buff_type, incoming_buff, recv_push_ptr, recv_wr);

	if (ENABLE_RECV_PRINTS || ENABLE_CREDIT_PRINTS || ENABLE_POST_RECV_PRINTS || ENABLE_STAT_COUNTING)
		if(packets_polled > 0){
			switch(buff_type){
				case ST_INV_BUFF:
					w_stats[worker_lid].received_invs_per_worker += reqs_polled;
					w_stats[worker_lid].received_packet_invs_per_worker += packets_polled;
					if(ENABLE_RECV_PRINTS && ENABLE_INV_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
						green_printf("^^^ Polled reqs[W%d]: %d packets \033[31mINVs\033[0m %d, (total packets: %d, reqs %d)!\n",
									 worker_lid, packets_polled, reqs_polled,
									 w_stats[worker_lid].received_packet_invs_per_worker, w_stats[worker_lid].received_invs_per_worker);
					if(ENABLE_CREDIT_PRINTS && ENABLE_ACK_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
						printf("$$$ Credits[W%d]: \033[33mACKs\033[0m \033[1m\033[32mincremented\033[0m "
									   "to %d (for machine %d)\n", worker_lid, credits[qp_credits_to_inc][sender], sender);
					if (ENABLE_POST_RECV_PRINTS && ENABLE_INV_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
						yellow_printf("vvv Post Receives[W%d]: \033[31mINVs\033[0m %d\n", worker_lid, packets_polled);
					if(ENABLE_ASSERTIONS)
						assert(INV_MAX_REQ_COALESCE != 1 || packets_polled == reqs_polled);
					break;
				case ST_ACK_BUFF:
					w_stats[worker_lid].received_acks_per_worker += reqs_polled;
					w_stats[worker_lid].received_packet_acks_per_worker += packets_polled;
					if(ENABLE_RECV_PRINTS && ENABLE_ACK_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
						green_printf("^^^ Polled reqs[W%d]: %d packets \033[33mACKs\033[0m %d, (total packets: %d, reqs: %d)!\n",
									 worker_lid, packets_polled, reqs_polled,
									 w_stats[worker_lid].received_packet_acks_per_worker, w_stats[worker_lid].received_acks_per_worker);
					if(ENABLE_CREDIT_PRINTS && ENABLE_INV_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
						printf("$$$ Credits[W%d]: \033[31mINVs\033[0m \033[1m\033[32mincremented\033[0m "
									   "to %d (for machine %d)\n", worker_lid, credits[qp_credits_to_inc][sender], sender);
					if (ENABLE_POST_RECV_PRINTS && ENABLE_ACK_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
						yellow_printf("vvv Post Receives[W%d]: \033[33mACKs\033[0m %d\n", worker_lid, packets_polled);
					if(ENABLE_ASSERTIONS)
						assert(ACK_MAX_REQ_COALESCE != 1 || packets_polled == reqs_polled);
					break;
				case ST_VAL_BUFF:
					w_stats[worker_lid].received_vals_per_worker += reqs_polled;
					w_stats[worker_lid].received_packet_vals_per_worker += packets_polled;
					if(ENABLE_RECV_PRINTS && ENABLE_VAL_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
						green_printf("^^^ Polled reqs[W%d]: %d packets \033[1m\033[32mVALs\033[0m %d, (total packets: %d reqs: %d)!\n",
									 worker_lid, packets_polled, reqs_polled,
									 w_stats[worker_lid].received_packet_vals_per_worker, w_stats[worker_lid].received_vals_per_worker);
					if(ENABLE_CREDIT_PRINTS && ENABLE_CRD_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
						printf("$$$ Credits[W%d]: \033[1m\033[36mCRDs\033[0m \033[1m\033[32mincremented\033[0m"
									   " to %d (for machine %d)\n", worker_lid, credits[qp_credits_to_inc][sender], sender);
					if(ENABLE_POST_RECV_PRINTS && ENABLE_VAL_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
						yellow_printf("vvv Post Receives[W%d]: \033[1m\033[32mVALs\033[0m %d\n", worker_lid, packets_polled);
					if(ENABLE_ASSERTIONS)
						assert(VAL_MAX_REQ_COALESCE != 1 || packets_polled == reqs_polled);
					break;
				default: assert(0);
			}
		}
}



/* ---------------------------------------------------------------------------
------------------------------------INVS--------------------------------------
---------------------------------------------------------------------------*/
static inline void
forge_bcast_inv_wrs(spacetime_op_t* op, spacetime_inv_packet_t* inv_send_op,
					struct ibv_send_wr* send_inv_wr, struct ibv_sge* send_inv_sgl,
					struct hrd_ctrl_blk* cb, long long* total_inv_bcasts,
					uint16_t br_i, uint16_t worker_lid)
{
	struct ibv_wc signal_send_wc;

	if(ENABLE_ASSERTIONS)
		assert(sizeof(spacetime_inv_t) == sizeof(spacetime_op_t));

	memcpy(&inv_send_op->reqs[inv_send_op->req_num], op, sizeof(spacetime_inv_t));
	inv_send_op->reqs[inv_send_op->req_num].opcode = ST_OP_INV;
	inv_send_op->reqs[inv_send_op->req_num].sender = (uint8_t) machine_id;
	inv_send_op->req_num++;

	w_stats[worker_lid].issued_invs_per_worker++;

	if(inv_send_op->req_num == 1) {
		send_inv_sgl[br_i].addr = (uint64_t) (uintptr_t) inv_send_op;
		int br_i_x_remotes = br_i * group_membership.num_of_alive_remotes;
		if(DISABLE_INV_INLINING)
			send_inv_wr[br_i_x_remotes].send_flags = 0;
		else
			send_inv_wr[br_i_x_remotes].send_flags = IBV_SEND_INLINE; //reset possibly signaled wr from the prev round
		// SET THE PREV SEND_WR TO POINT TO CURR
		if (br_i > 0)
			send_inv_wr[br_i_x_remotes - 1].next = &send_inv_wr[br_i_x_remotes];
		// Do a Signaled Send every INV_SS_GRANULARITY broadcasts (INV_SS_GRANULARITY * REMOTE_MACHINES messages)
		if (*total_inv_bcasts % INV_SS_GRANULARITY == 0) {
			if(*total_inv_bcasts > 0) { //if not the first SS poll the previous SS completion
				hrd_poll_cq(cb->dgram_send_cq[INV_UD_QP_ID], 1, &signal_send_wc);
				w_stats[worker_lid].inv_ss_completions_per_worker++;
				if (ENABLE_SS_PRINTS && ENABLE_INV_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
					red_printf( "^^^ Polled SS completion[W%d]: \033[31mINV\033[0m %d "
										"(total ss comp: %d --> reqs comp: %d, curr_invs: %d)\n",
								worker_lid, 1, w_stats[worker_lid].inv_ss_completions_per_worker,
								(*total_inv_bcasts - INV_SS_GRANULARITY) * REMOTE_MACHINES + 1,
								*total_inv_bcasts * REMOTE_MACHINES);
			}
			if (ENABLE_SS_PRINTS && ENABLE_INV_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
				red_printf("vvv Send SS[W%d]: \033[31mINV\033[0m \n", worker_lid);
			send_inv_wr[br_i_x_remotes].send_flags |= IBV_SEND_SIGNALED;
		}
		(*total_inv_bcasts)++;
	}
}

static inline void
batch_invs_2_NIC(struct ibv_send_wr *send_inv_wr, struct ibv_sge *send_inv_sgl,
				 struct hrd_ctrl_blk *cb, uint16_t br_i,
				 uint16_t total_invs_in_batch, uint16_t worker_lid)
{
	int j = 0, ret, k = 0;
	struct ibv_send_wr *bad_send_wr;

	w_stats[worker_lid].issued_packet_invs_per_worker += br_i;
	send_inv_wr[br_i * group_membership.num_of_alive_remotes - 1].next = NULL;
	if (ENABLE_ASSERTIONS) {
		int sgl_index = 0;
		for (j = 0; j < br_i * group_membership.num_of_alive_remotes - 1; j++) {
			sgl_index = j / group_membership.num_of_alive_remotes;
//			sgl_index = j / REMOTE_MACHINES;
			assert(send_inv_wr[j].num_sge == 1);
			assert(send_inv_wr[j].next == &send_inv_wr[j + 1]);
			assert(DISABLE_INV_INLINING || send_inv_wr[j].opcode == IBV_SEND_INLINE | IBV_SEND_SIGNALED);
			assert(send_inv_wr[j].sg_list == &send_inv_sgl[sgl_index]);
			assert(send_inv_sgl[sgl_index].length == sizeof(spacetime_inv_packet_t));
			assert(((spacetime_inv_packet_t *) send_inv_sgl[sgl_index].addr)->req_num > 0);
			for(k = 0; k < ((spacetime_inv_packet_t *) send_inv_sgl[sgl_index].addr)->req_num; k ++){
				assert(((spacetime_inv_packet_t *) send_inv_sgl[sgl_index].addr)->reqs[k].opcode == ST_OP_INV);
				assert(((spacetime_inv_packet_t *) send_inv_sgl[sgl_index].addr)->reqs[k].sender == machine_id);
				assert(group_membership.num_of_alive_remotes != REMOTE_MACHINES ||
					   ((spacetime_inv_packet_t *) send_inv_sgl[sgl_index].addr)->reqs[k].tie_breaker_id == machine_id);
			}
//			green_printf("Ops[%d]--> hash(1st 8B):%" PRIu64 "\n",
//						 j, ((uint64_t *) &((spacetime_inv_packet_t*) send_inv_sgl[sgl_index].addr)->reqs[0].key)[1]);
		}
//        green_printf("Ops[%d]--> hash(1st 8B):%" PRIu64 "\n",
//                     j, ((uint64_t *) &((spacetime_inv_packet_t*) send_inv_sgl[sgl_index].addr)->reqs[0].key)[1]);
		assert(send_inv_wr[j].next == NULL);
	}

	if (ENABLE_SEND_PRINTS && ENABLE_INV_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
		cyan_printf(">>> Send[W%d]: %d bcast %d packets \033[31mINVs\033[0m (%d) (Total bcasts: %d, packets %d, invs: %d)\n",
					worker_lid, total_invs_in_batch, br_i, total_invs_in_batch * REMOTE_MACHINES,
					w_stats[worker_lid].issued_invs_per_worker, w_stats[worker_lid].issued_packet_invs_per_worker,
					w_stats[worker_lid].issued_invs_per_worker * REMOTE_MACHINES);
	ret = ibv_post_send(cb->dgram_qp[INV_UD_QP_ID], &send_inv_wr[0], &bad_send_wr);
	if (ret)
		printf("Total invs issued:%llu \n", w_stats[worker_lid].issued_invs_per_worker * REMOTE_MACHINES);
	CPE(ret, "INVs ibv_post_send error", ret);
}

static inline void
broadcast_invs(spacetime_op_t *ops, spacetime_inv_packet_t *inv_send_packet_ops,
			   int *inv_push_ptr, struct ibv_send_wr *send_inv_wr,
			   struct ibv_sge *send_inv_sgl, uint8_t credits[][MACHINE_NUM],
			   struct hrd_ctrl_blk *cb, long long *total_inv_bcasts,
			   uint16_t worker_lid, spacetime_group_membership last_g_membership,
			   int* node_missing_credits, uint32_t* credits_missing, uint16_t *rolling_index)
{
	uint8_t missing_credits = 0;
	uint16_t i = 0, br_i = 0, j = 0, total_invs_in_batch = 0, index = 0;

	if(ENABLE_ASSERTIONS)
		assert(inv_send_packet_ops[*inv_push_ptr].req_num == 0);

	// traverse all of the ops to find invs
	for (i = 0; i < MAX_BATCH_OPS_SIZE; i++) {
		index = (uint16_t) ((i + *rolling_index) % MAX_BATCH_OPS_SIZE);
		if (ops[index].state != ST_PUT_SUCCESS && ops[index].state != ST_REPLAY_SUCCESS)
			continue;

		//Check for credits
		for (j = 0; j < MACHINE_NUM; j++) {
			if (j == machine_id) continue; // skip the local machine
            if (!node_is_in_membership(last_g_membership, j)) continue; //skip machine which is removed from group
			if (credits[INV_UD_QP_ID][j] == 0) {
				missing_credits = 1;
				*rolling_index = index;
				credits_missing[j]++;
                if(unlikely(credits_missing[j] > M_1))
					*node_missing_credits = j;
				break;
			}else
				credits_missing[j] = 0;
		}
		// if not enough credits for a Broadcast
		if (missing_credits == 1) break;

		for (j = 0; j < MACHINE_NUM; j++){
			if (j == machine_id) continue; // skip the local machine
			if (!node_is_in_membership(last_g_membership, j)) continue; //skip machine which is removed from group
			credits[INV_UD_QP_ID][j]--;
            if (ENABLE_CREDIT_PRINTS && ENABLE_INV_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
				printf("$$$ Credits[W%d]: \033[31mINVs\033[0m \033[31mdecremented\033[0m to %d (for machine %d)\n",
					   worker_lid, credits[INV_UD_QP_ID][j], j);
		}

		//Change state of op
		if(ops[index].state == ST_PUT_SUCCESS)
			ops[index].state = ST_IN_PROGRESS_PUT;
		else if(ops[index].state == ST_REPLAY_SUCCESS)
			ops[index].state = ST_IN_PROGRESS_REPLAY;
		else assert(0);

		// Create the broadcast messages
		forge_bcast_inv_wrs(&ops[index], &inv_send_packet_ops[*inv_push_ptr],
							send_inv_wr, send_inv_sgl, cb, total_inv_bcasts, br_i, worker_lid);
		total_invs_in_batch++;
		// if packet is full
		if(inv_send_packet_ops[*inv_push_ptr].req_num == INV_MAX_REQ_COALESCE) {
			br_i++;
			if (br_i == MAX_PCIE_BCAST_BATCH) { //check if we should batch it to NIC
				batch_invs_2_NIC(send_inv_wr, send_inv_sgl, cb, br_i,
								 total_invs_in_batch, worker_lid);
				br_i = 0;
				total_invs_in_batch = 0;
			}
			HRD_MOD_ADD(*inv_push_ptr, INV_SEND_OPS_SIZE / REMOTE_MACHINES * group_membership.num_of_alive_remotes); //got to the next "packet" + dealing with failutes
			//Reset data left from previous bcasts after ibv_post_send to avoid sending reseted data
			inv_send_packet_ops[*inv_push_ptr].req_num = 0;
			for(j = 0; j < INV_MAX_REQ_COALESCE; j++)
				inv_send_packet_ops[*inv_push_ptr].reqs[j].opcode = ST_EMPTY;
		}
	}


	if(inv_send_packet_ops[*inv_push_ptr].req_num > 0 &&
	   inv_send_packet_ops[*inv_push_ptr].req_num < INV_MAX_REQ_COALESCE)
		br_i++;

	if(br_i > 0)
		batch_invs_2_NIC(send_inv_wr, send_inv_sgl, cb, br_i,
						 total_invs_in_batch, worker_lid);

	//Move to next packet and reset data left from previous bcasts
	if(inv_send_packet_ops[*inv_push_ptr].req_num > 0 &&
	   inv_send_packet_ops[*inv_push_ptr].req_num < INV_MAX_REQ_COALESCE) {
		HRD_MOD_ADD(*inv_push_ptr, INV_SEND_OPS_SIZE / REMOTE_MACHINES * group_membership.num_of_alive_remotes); //got to the next "packet" + dealing with failutes
		inv_send_packet_ops[*inv_push_ptr].req_num = 0;
		for(j = 0; j < INV_MAX_REQ_COALESCE; j++)
			inv_send_packet_ops[*inv_push_ptr].reqs[j].opcode = ST_EMPTY;
	}

	if(ENABLE_ASSERTIONS)
		for(i = 0; i < MAX_BATCH_OPS_SIZE; i++)
			assert(ops[i].opcode == ST_OP_GET              ||
				   ops[i].state == ST_PUT_STALL            ||
				   ops[i].state == ST_PUT_SUCCESS          ||
				   ops[i].state == ST_IN_PROGRESS_PUT      ||
				   ops[i].state == ST_REPLAY_COMPLETE      ||
				   ops[i].state == ST_IN_PROGRESS_REPLAY   ||
				   ops[i].state == ST_PUT_COMPLETE_SEND_VALS);

}

/* ---------------------------------------------------------------------------
------------------------------------ACKS--------------------------------------
---------------------------------------------------------------------------*/

static inline void
forge_ack_wr(spacetime_inv_t* inv_recv_op, spacetime_ack_packet_t* ack_send_ops,
			 struct ibv_send_wr* send_ack_wr, struct ibv_sge* send_ack_sgl,
			 struct hrd_ctrl_blk* cb, long long* send_ack_tx,
			 uint16_t send_ack_packets, uint16_t worker_lid)
{
	struct ibv_wc signal_send_wc;
	uint16_t dst_worker_gid = (uint16_t) (inv_recv_op->sender * WORKERS_PER_MACHINE + worker_lid);
	if(ENABLE_ASSERTIONS)
		assert(REMOTE_MACHINES != 1 || inv_recv_op->sender == REMOTE_MACHINES - machine_id);

	memcpy(&ack_send_ops->reqs[ack_send_ops->req_num], inv_recv_op, sizeof(spacetime_ack_t));
	ack_send_ops->reqs[ack_send_ops->req_num].opcode = ST_OP_ACK;
	ack_send_ops->reqs[ack_send_ops->req_num].sender = (uint8_t) machine_id;
	ack_send_ops->req_num++;
	if(ENABLE_ASSERTIONS)
		assert(ack_send_ops->req_num <= ACK_MAX_REQ_COALESCE);

	w_stats[worker_lid].issued_acks_per_worker++;

	if(ack_send_ops->req_num == 1) {
		send_ack_sgl[send_ack_packets].addr = (uint64_t) ack_send_ops;
		send_ack_wr[send_ack_packets].wr.ud.ah = remote_worker_qps[dst_worker_gid][ACK_UD_QP_ID].ah;
		send_ack_wr[send_ack_packets].wr.ud.remote_qpn = (uint32) remote_worker_qps[dst_worker_gid][ACK_UD_QP_ID].qpn;
		if(DISABLE_ACK_INLINING)
			send_ack_wr[send_ack_packets].send_flags = 0;
		else
			send_ack_wr[send_ack_packets].send_flags = IBV_SEND_INLINE;

		if (send_ack_packets > 0)
			send_ack_wr[send_ack_packets - 1].next = &send_ack_wr[send_ack_packets];

		// Do a Signaled Send every ACK_SS_GRANULARITY msgs
		if (*send_ack_tx % ACK_SS_GRANULARITY == 0) {
			if(*send_ack_tx > 0){ //if not the first SS poll the previous SS completion
				hrd_poll_cq(cb->dgram_send_cq[ACK_UD_QP_ID], 1, &signal_send_wc);
				w_stats[worker_lid].ack_ss_completions_per_worker++;
				if (ENABLE_SS_PRINTS && ENABLE_ACK_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
					red_printf("^^^ Polled SS completion[W%d]: \033[33mACK\033[0m %d (total %d)\n", worker_lid, 1,
							   w_stats[worker_lid].ack_ss_completions_per_worker);
			}
			send_ack_wr[send_ack_packets].send_flags |= IBV_SEND_SIGNALED;
			if (ENABLE_SS_PRINTS && ENABLE_ACK_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
				red_printf("vvv Send SS[W%d]: \033[33mACK\033[0m\n", worker_lid);
		}
		(*send_ack_tx)++;
	}
}

static inline void
batch_acks_2_NIC(struct ibv_send_wr *send_ack_wr, struct ibv_sge *send_ack_sgl,
				 struct hrd_ctrl_blk *cb, uint16_t send_ack_packets,
				 uint16_t total_acks_in_batch, uint16_t worker_lid)
{
	int j = 0, ret;
	struct ibv_send_wr *bad_send_wr;

	w_stats[worker_lid].issued_packet_acks_per_worker += send_ack_packets;
	send_ack_wr[send_ack_packets - 1].next = NULL;
	if(ENABLE_ASSERTIONS){
		for(j = 0; j < send_ack_packets - 1; j++){
			assert(send_ack_wr[j].next == &send_ack_wr[j+1]);
			assert(send_ack_wr[j].opcode == IBV_WR_SEND);
			assert(send_ack_wr[j].wr.ud.remote_qkey == HRD_DEFAULT_QKEY);
			assert(send_ack_wr[j].sg_list == &send_ack_sgl[j]);
			assert(send_ack_wr[j].sg_list->length == sizeof(spacetime_ack_packet_t));
			assert(send_ack_wr[j].num_sge == 1);
			assert(DISABLE_ACK_INLINING || send_ack_wr[j].send_flags == IBV_SEND_INLINE ||
				   send_ack_wr[j].send_flags == IBV_SEND_INLINE | IBV_SEND_SIGNALED);
			assert(((spacetime_ack_packet_t *) send_ack_sgl[j].addr)->req_num > 0);
			assert(((spacetime_ack_packet_t *) send_ack_sgl[j].addr)->reqs[0].opcode == ST_OP_ACK);
			assert(((spacetime_ack_packet_t *) send_ack_sgl[j].addr)->reqs[0].sender == machine_id);
		}
		assert(send_ack_wr[j].next == NULL);
	}

	if (ENABLE_SEND_PRINTS && ENABLE_ACK_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
		cyan_printf(">>> Send[W%d]: %d packets \033[33mACKs\033[0m %d (Total packets: %d, acks: %d)\n",
					worker_lid, send_ack_packets, total_acks_in_batch,
					w_stats[worker_lid].issued_packet_acks_per_worker,
					w_stats[worker_lid].issued_acks_per_worker);
	ret = ibv_post_send(cb->dgram_qp[ACK_UD_QP_ID], &send_ack_wr[0], &bad_send_wr);
	CPE(ret, "ibv_post_send error while sending ACKs", ret);
}

static inline void
issue_acks(spacetime_inv_t *inv_recv_ops, spacetime_ack_packet_t* ack_send_packet_ops,
		   int* ack_push_ptr, long long int* send_ack_tx, struct ibv_send_wr *send_ack_wr,
		   struct ibv_sge *send_ack_sgl, uint8_t credits[][MACHINE_NUM],
		   struct hrd_ctrl_blk *cb,  uint16_t worker_lid)
{
	uint16_t i = 0, total_acks_in_batch = 0, j = 0, send_ack_packets = 0;
	uint8_t last_ack_dst = 255;

	if(ENABLE_ASSERTIONS)
		assert(ack_send_packet_ops[*ack_push_ptr].req_num == 0);

	for (i = 0; i < INV_RECV_OPS_SIZE; i++) {
		if (inv_recv_ops[i].opcode == ST_EMPTY)
			break;

		if(ENABLE_ASSERTIONS){
			assert(inv_recv_ops[i].opcode == ST_INV_SUCCESS || inv_recv_ops[i].opcode == ST_OP_INV);
			assert(inv_recv_ops[i].val_len == ST_VALUE_SIZE);
			assert(REMOTE_MACHINES != 1 || inv_recv_ops[i].tie_breaker_id == REMOTE_MACHINES - machine_id);
			assert(REMOTE_MACHINES != 1 || inv_recv_ops[i].value[0] == (uint8_t) 'x' + (REMOTE_MACHINES - machine_id));
			assert(REMOTE_MACHINES != 1 || inv_recv_ops[i].sender == REMOTE_MACHINES - machine_id);
		}

		if (credits[ACK_UD_QP_ID][inv_recv_ops[i].sender] == 0)
			assert(0); // we should always have credits for acks

		//reduce credits
		credits[ACK_UD_QP_ID][inv_recv_ops[i].sender]--;
		if (ENABLE_CREDIT_PRINTS && ENABLE_ACK_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
			printf("$$$ Credits[W%d]: \033[33mACKs\033[0m \033[31mdecremented\033[0m to %d (for machine %d)\n",
				   worker_lid, credits[ACK_UD_QP_ID][inv_recv_ops[i].sender], inv_recv_ops[i].sender);

		if(ack_send_packet_ops[*ack_push_ptr].req_num != 0 && inv_recv_ops[i].sender != last_ack_dst){ //in case that the last ack dst != this ack dst
			send_ack_packets++;
			if (send_ack_packets == MAX_SEND_ACK_WRS) {
				batch_acks_2_NIC(send_ack_wr, send_ack_sgl, cb, send_ack_packets,
								 total_acks_in_batch, worker_lid);
				send_ack_packets = 0;
				total_acks_in_batch = 0;
			}
			HRD_MOD_ADD(*ack_push_ptr, ACK_SEND_OPS_SIZE); //got to the next "packet"
			//Reset data left from previous unicasts
			ack_send_packet_ops[*ack_push_ptr].req_num = 0;
			for(j = 0; j < ACK_MAX_REQ_COALESCE; j++)
				ack_send_packet_ops[*ack_push_ptr].reqs[j].opcode = ST_EMPTY;
		}
		last_ack_dst = inv_recv_ops[i].sender;


		//empty inv buffer
		if(inv_recv_ops[i].opcode == ST_INV_SUCCESS)
			inv_recv_ops[i].opcode = ST_EMPTY;
		else assert(0);

		// Create the broadcast messages
		forge_ack_wr(&inv_recv_ops[i], &ack_send_packet_ops[*ack_push_ptr], send_ack_wr,
					 send_ack_sgl, cb, send_ack_tx, send_ack_packets, worker_lid);
		total_acks_in_batch++;

		if(ack_send_packet_ops[*ack_push_ptr].req_num == ACK_MAX_REQ_COALESCE) {
			send_ack_packets++;
			if (send_ack_packets == MAX_SEND_ACK_WRS) {
				batch_acks_2_NIC(send_ack_wr, send_ack_sgl, cb, send_ack_packets,
								 total_acks_in_batch, worker_lid);
				send_ack_packets = 0;
				total_acks_in_batch = 0;
			}
			HRD_MOD_ADD(*ack_push_ptr, ACK_SEND_OPS_SIZE);
			//Reset data left from previous unicasts
			ack_send_packet_ops[*ack_push_ptr].req_num = 0;
			for(j = 0; j < ACK_MAX_REQ_COALESCE; j++)
				ack_send_packet_ops[*ack_push_ptr].reqs[j].opcode = ST_EMPTY;

		}
	}

	if(ack_send_packet_ops[*ack_push_ptr].req_num > 0 &&
	   ack_send_packet_ops[*ack_push_ptr].req_num < ACK_MAX_REQ_COALESCE)
		send_ack_packets++;

	if (send_ack_packets > 0)
		batch_acks_2_NIC(send_ack_wr, send_ack_sgl, cb, send_ack_packets,
						 total_acks_in_batch, worker_lid);

	//Move to next packet and reset data left from previous unicasts
	if(ack_send_packet_ops[*ack_push_ptr].req_num > 0 &&
	   ack_send_packet_ops[*ack_push_ptr].req_num < ACK_MAX_REQ_COALESCE) {
		HRD_MOD_ADD(*ack_push_ptr, ACK_SEND_OPS_SIZE);
		ack_send_packet_ops[*ack_push_ptr].req_num = 0;
		for(j = 0; j < ACK_MAX_REQ_COALESCE; j++)
			ack_send_packet_ops[*ack_push_ptr].reqs[j].opcode = ST_EMPTY;
	}

	if(ENABLE_ASSERTIONS)
		for(i = 0; i < INV_RECV_OPS_SIZE; i++)
			assert(inv_recv_ops[i].opcode == ST_EMPTY);
}


/* ---------------------------------------------------------------------------
------------------------------------VALs--------------------------------------
---------------------------------------------------------------------------*/

static inline void
post_credit_recvs(struct hrd_ctrl_blk *cb, struct ibv_recv_wr *credit_recv_wr,
				  uint16_t num_recvs)
{
	uint16_t i;
	int ret;
	struct ibv_recv_wr *bad_recv_wr;
	for (i = 0; i < num_recvs; i++)
		credit_recv_wr[i].next = (i == num_recvs - 1) ? NULL : &credit_recv_wr[i + 1];
	ret = ibv_post_recv(cb->dgram_qp[CRD_UD_QP_ID], &credit_recv_wr[0], &bad_recv_wr);
	CPE(ret, "ibv_post_recv error: posting recvs for credits before val bcast", ret);
}

// Poll and increment credits
static inline void
poll_cq_for_credits(struct ibv_cq *credit_recv_cq, struct ibv_wc *credit_wc,
					struct ibv_recv_wr* credit_recv_wr, struct hrd_ctrl_blk* cb,
					uint8_t credits[][MACHINE_NUM], uint16_t worker_lid)
{
	spacetime_crd_t* crd_ptr;
	int i = 0, credits_found = 0;
	credits_found = ibv_poll_cq(credit_recv_cq, MAX_RECV_CRD_WRS, credit_wc);
	if(credits_found > 0) {
		if(unlikely(credit_wc[credits_found -1].status != 0)) {
			fprintf(stderr, "Bad wc status when polling for credits to send a broadcast %d\n", credit_wc[credits_found -1].status);
			exit(0);
		}

		w_stats[worker_lid].received_packet_crds_per_worker += credits_found;
		if(ENABLE_RECV_PRINTS && ENABLE_CRD_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
			green_printf("^^^ Polled reqs[W%d]: \033[1m\033[36mCRDs\033[0m %d, (total: %d)!\n",worker_lid, credits_found, w_stats[worker_lid].received_crds_per_worker);
		for (i = 0; i < credits_found; i++){
			crd_ptr = (spacetime_crd_t*) &credit_wc[i].imm_data;
			if(ENABLE_ASSERTIONS){
				assert(crd_ptr->opcode == ST_OP_CRD);
				assert(REMOTE_MACHINES != 1 || crd_ptr->sender == REMOTE_MACHINES - machine_id);
			}
			w_stats[worker_lid].received_crds_per_worker += crd_ptr->val_credits;
			credits[VAL_UD_QP_ID][crd_ptr->sender] += crd_ptr->val_credits;
			if(ENABLE_ASSERTIONS)
				assert(credits[VAL_UD_QP_ID][crd_ptr->sender] <= VAL_CREDITS);
			if(ENABLE_CREDIT_PRINTS && ENABLE_VAL_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
				printf("$$$ Credits[W%d]: \033[1m\033[32mVALs\033[0m \033[1m\033[32mincremented\033[0m to %d (for machine %d)\n",
					   worker_lid, credits[VAL_UD_QP_ID][crd_ptr->sender], crd_ptr->sender);
		}
		if (ENABLE_POST_RECV_PRINTS && ENABLE_CRD_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
			yellow_printf("vvv Post Receives[W%d]: \033[1m\033[36mCRDs\033[0m %d\n", worker_lid, credits_found);
		post_credit_recvs(cb, credit_recv_wr, (uint16_t) credits_found);
	} else if(unlikely(credits_found < 0)) {
		printf("ERROR In the credit CQ\n");
		exit(0);
	}
}

static inline bool
check_val_credits(uint8_t credits[][MACHINE_NUM], struct hrd_ctrl_blk *cb,
				  struct ibv_wc *credit_wc, struct ibv_recv_wr* credit_recv_wr,
                  spacetime_group_membership last_g_membership,
//				  int* node_missing_credits, uint32_t *credits_missing,
				  uint16_t worker_lid)
{
	uint16_t poll_for_credits = 0, j;
	for (j = 0; j < MACHINE_NUM; j++) {
		if (j == machine_id) continue; // skip the local machine
		if (!node_is_in_membership(last_g_membership, j)) continue; //skip machine which is removed from group
		if (credits[VAL_UD_QP_ID][j] == 0) {
			poll_for_credits = 1;
			break;
		}
// else
//			credits_missing[j] = 0;
	}

	if (poll_for_credits == 1) {
		poll_cq_for_credits(cb->dgram_recv_cq[CRD_UD_QP_ID], credit_wc, credit_recv_wr, cb, credits, worker_lid);
		// We polled for credits, if we did not find enough just break
		for (j = 0; j < MACHINE_NUM; j++) {
			if (j == machine_id) continue; // skip the local machine
			if (!node_is_in_membership(last_g_membership, j)) continue; //skip machine which is removed from group
			if (credits[VAL_UD_QP_ID][j] == 0) {
//				credits_missing[j]++;
//				if(unlikely(credits_missing[j] > M_1))
//					*node_missing_credits = j;
				return false;
			}
//			else
//				credits_missing[j] = 0;
		}
	}

	return poll_for_credits == 1 ? false : true;
}

static inline void
//forge_bcast_val_wrs(spacetime_ack_t* op, spacetime_val_packet_t* val_packet_send_op,
forge_bcast_val_wrs(void* op, spacetime_val_packet_t* val_packet_send_op,
					struct ibv_send_wr* send_val_wr,
					struct ibv_sge* send_val_sgl, struct hrd_ctrl_blk* cb,
					long long* total_val_bcasts, uint16_t br_i, uint16_t worker_lid)
{
	struct ibv_wc signal_send_wc;
	if(ENABLE_ASSERTIONS)
		assert(sizeof(spacetime_ack_t) == sizeof(spacetime_val_t));

	//WARNING ack_op is used to forge from both spacetime_ack_t and spacetime_op_t (when failures occur)--> do not change those structs
	memcpy(&val_packet_send_op->reqs[val_packet_send_op->req_num], op, sizeof(spacetime_val_t));
	val_packet_send_op->reqs[val_packet_send_op->req_num].opcode = ST_OP_VAL;
	val_packet_send_op->reqs[val_packet_send_op->req_num].sender = (uint8_t) machine_id;
	val_packet_send_op->req_num++;

	w_stats[worker_lid].issued_vals_per_worker++;

	if(val_packet_send_op->req_num == 1) {
		int br_i_x_remotes = br_i * group_membership.num_of_alive_remotes;
		send_val_sgl[br_i].addr = (uint64_t) (uintptr_t) val_packet_send_op;
		if(DISABLE_VAL_INLINING)
			send_val_wr[br_i_x_remotes].send_flags = 0;
		else
			send_val_wr[br_i_x_remotes].send_flags = IBV_SEND_INLINE;

		// SET THE LAST SEND_WR TO POINT TO NULL
		if (br_i > 0)
			send_val_wr[br_i_x_remotes - 1].next = &send_val_wr[br_i_x_remotes];
		// Do a Signaled Send every VAL_SS_GRANULARITY broadcasts (VAL_SS_GRANULARITY * (MACHINE_NUM - 1) messages)
		if (*total_val_bcasts % VAL_SS_GRANULARITY == 0) {
			if(*total_val_bcasts > 0){
				hrd_poll_cq(cb->dgram_send_cq[VAL_UD_QP_ID], 1, &signal_send_wc);
				w_stats[worker_lid].val_ss_completions_per_worker++;
				if (ENABLE_SS_PRINTS && ENABLE_VAL_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
					red_printf("^^^ Polled SS completion[W%d]: \033[1m\033[32mVAL\033[0m %d (total %d)"
									   "(total ss comp: %d --> reqs comp: %d, curr_val: %d)\n",
							   worker_lid, 1, w_stats[worker_lid].val_ss_completions_per_worker,
							   (*total_val_bcasts - VAL_SS_GRANULARITY) * REMOTE_MACHINES + 1,
							   *total_val_bcasts * REMOTE_MACHINES);
			}
			if (ENABLE_SS_PRINTS && ENABLE_VAL_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
				red_printf("vvv Send SS[W%d]: \033[1m\033[32mVAL\033[0m \n", worker_lid);
			send_val_wr[br_i_x_remotes].send_flags |= IBV_SEND_SIGNALED;
		}
		(*total_val_bcasts)++;
	}
}


static inline void
batch_vals_2_NIC(struct ibv_send_wr *send_val_wr, struct ibv_sge *send_val_sgl,
				 struct hrd_ctrl_blk *cb, uint16_t br_i,
				 uint16_t total_vals_in_batch, uint16_t worker_lid)
{
	int j = 0, ret;
	struct ibv_send_wr *bad_send_wr;

	w_stats[worker_lid].issued_packet_vals_per_worker += br_i;
	send_val_wr[br_i * group_membership.num_of_alive_remotes - 1].next = NULL;
	if (ENABLE_ASSERTIONS) {
		int sgl_index;
		for (j = 0; j < br_i * group_membership.num_of_alive_remotes - 1; j++) {
			sgl_index = j / group_membership.num_of_alive_remotes;
//			sgl_index = j / REMOTE_MACHINES;
			assert(send_val_wr[j].next == &send_val_wr[j + 1]);
			assert(send_val_wr[j].num_sge == 1);
			assert(DISABLE_VAL_INLINING || send_val_wr[j].opcode == IBV_SEND_INLINE | IBV_SEND_SIGNALED);
			assert(send_val_wr[j].sg_list == &send_val_sgl[sgl_index]);
			assert(send_val_sgl[sgl_index].length == sizeof(spacetime_val_packet_t));
		}
		assert(send_val_wr[j].next == NULL);
	}

	if (ENABLE_SEND_PRINTS && ENABLE_VAL_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
		cyan_printf( ">>> Send[W%d]: %d bcast %d packets \033[1m\033[32mVALs\033[0m (%d) (Total bcasts: %d, packets: %d, vals: %d)\n",
					 worker_lid, total_vals_in_batch, br_i, total_vals_in_batch * REMOTE_MACHINES,
					 w_stats[worker_lid].issued_vals_per_worker, w_stats[worker_lid].issued_packet_vals_per_worker, w_stats[worker_lid].issued_vals_per_worker * REMOTE_MACHINES);
	ret = ibv_post_send(cb->dgram_qp[VAL_UD_QP_ID], &send_val_wr[0], &bad_send_wr);
	if (ret)
		printf("Total vals issued: %llu\n", w_stats[worker_lid].issued_vals_per_worker);
	CPE(ret, "1st: Broadcast VALs ibv_post_send error", ret);
}

static inline uint8_t
broadcast_vals(spacetime_ack_t *ack_ops, spacetime_val_packet_t *val_send_packet_ops, int *val_push_ptr,
			   struct ibv_send_wr *send_val_wr, struct ibv_sge *send_val_sgl,
			   uint8_t credits[][MACHINE_NUM], struct hrd_ctrl_blk *cb,
			   struct ibv_wc *credit_wc, spacetime_group_membership last_g_membership,
			   long long *br_tx, struct ibv_recv_wr *credit_recv_wr, uint16_t worker_lid)
{
	uint16_t i = 0, br_i = 0, j, total_vals_in_batch = 0;
	uint8_t has_outstanding_vals = 0;

	if(ENABLE_ASSERTIONS)
		assert(val_send_packet_ops[*val_push_ptr].req_num == 0);

	// traverse all of the ops to find bcasts
	for (i = 0; i < ACK_RECV_OPS_SIZE; i++) {
		if (ack_ops[i].opcode == ST_ACK_SUCCESS) {
			ack_ops[i].opcode = ST_EMPTY;
			continue;
		} else if (ack_ops[i].opcode == ST_EMPTY)
			continue;

		if(ENABLE_ASSERTIONS)
			assert(ack_ops[i].opcode == ST_LAST_ACK_SUCCESS);

		// if not enough credits for a Broadcast
		if (!check_val_credits(credits, cb, credit_wc, credit_recv_wr, last_g_membership, worker_lid)){
			has_outstanding_vals = 1;
			break;
		}

		for (j = 0; j < MACHINE_NUM; j++){
			if (j == machine_id) continue; // skip the local machine
			if (!node_is_in_membership(last_g_membership, j)) continue; //skip machine which is removed from group
			credits[VAL_UD_QP_ID][j]--;
            if (ENABLE_CREDIT_PRINTS && ENABLE_VAL_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
				printf("$$$ Credits[W%d]: \033[1m\033[32mVALs\033[0m \033[31mdecremented\033[0m to %d (for machine %d)\n",
					   worker_lid, credits[VAL_UD_QP_ID][j], j);
		}

		ack_ops[i].opcode = ST_EMPTY;

		// Create the broadcast messages
		forge_bcast_val_wrs(&ack_ops[i], &val_send_packet_ops[*val_push_ptr], send_val_wr,
							send_val_sgl, cb, br_tx, br_i, worker_lid);
		total_vals_in_batch++;

		if(val_send_packet_ops[*val_push_ptr].req_num == VAL_MAX_REQ_COALESCE) {
			br_i++;
			if (br_i == MAX_PCIE_BCAST_BATCH) {
				batch_vals_2_NIC(send_val_wr, send_val_sgl, cb, br_i,
								 total_vals_in_batch, worker_lid);
				br_i = 0;
				total_vals_in_batch = 0;
			}
			HRD_MOD_ADD(*val_push_ptr, VAL_SEND_OPS_SIZE / REMOTE_MACHINES * group_membership.num_of_alive_remotes); //got to the next "packet" + dealing with failutes
			//Reset data left from previous bcasts after ibv_post_send to avoid sending reseted data
			val_send_packet_ops[*val_push_ptr].req_num = 0;
			for(j = 0; j < VAL_MAX_REQ_COALESCE; j++)
				val_send_packet_ops[*val_push_ptr].reqs[j].opcode = ST_EMPTY;
		}
	}

	if(val_send_packet_ops[*val_push_ptr].req_num > 0 &&
	   val_send_packet_ops[*val_push_ptr].req_num < VAL_MAX_REQ_COALESCE)
		br_i++;

	if(br_i > 0)
		batch_vals_2_NIC(send_val_wr, send_val_sgl, cb, br_i,
						 total_vals_in_batch, worker_lid);

	//Reset data left from previous bcasts
	if(val_send_packet_ops[*val_push_ptr].req_num > 0 &&
	   val_send_packet_ops[*val_push_ptr].req_num < VAL_MAX_REQ_COALESCE) {
		HRD_MOD_ADD(*val_push_ptr, VAL_SEND_OPS_SIZE / REMOTE_MACHINES * group_membership.num_of_alive_remotes); //got to the next "packet" + dealing with failutes
		val_send_packet_ops[*val_push_ptr].req_num = 0;
		for(j = 0; j < VAL_MAX_REQ_COALESCE; j++)
			val_send_packet_ops[*val_push_ptr].reqs[j].opcode = ST_EMPTY;
	}

	if (ENABLE_ASSERTIONS && has_outstanding_vals == 0)
		for (i = 0; i < ACK_RECV_OPS_SIZE; i++)
			assert(ack_ops[i].opcode == ST_EMPTY);

	return has_outstanding_vals;
}


static inline uint8_t
broadcast_vals_on_membership_change
		(spacetime_op_t* ops, spacetime_val_packet_t* val_send_packet_ops,
		 int* val_push_ptr, struct ibv_send_wr* send_val_wr, struct ibv_sge* send_val_sgl,
		 uint8_t credits[][MACHINE_NUM], struct hrd_ctrl_blk* cb,
		 struct ibv_wc* credit_wc, spacetime_group_membership last_g_membership,
		 long long* br_tx, struct ibv_recv_wr* credit_recv_wr, uint16_t worker_lid)
{
	uint16_t i = 0, br_i = 0, j, total_vals_in_batch = 0;
	uint8_t has_outstanding_vals = 0;

	if(ENABLE_ASSERTIONS)
		assert(val_send_packet_ops[*val_push_ptr].req_num == 0);

	// traverse all of the ops to find bcasts
	for (i = 0; i < MAX_BATCH_OPS_SIZE; i++) {
		if(ops[i].state != ST_REPLAY_COMPLETE &&
		   ops[i].state != ST_PUT_COMPLETE_SEND_VALS)
			continue;

		// if not enough credits for a Broadcast
		if (!check_val_credits(credits, cb, credit_wc, credit_recv_wr, last_g_membership, worker_lid)){
			has_outstanding_vals = 1;
			break;
		}

		for (j = 0; j < MACHINE_NUM; j++){
			if (j == machine_id) continue; // skip the local machine
			if (!node_is_in_membership(last_g_membership, j)) continue; //skip machine which is removed from group
			credits[VAL_UD_QP_ID][j]--;
            if (ENABLE_CREDIT_PRINTS && ENABLE_VAL_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
			printf("$$$ Credits[W%d]: \033[1m\033[32mVALs\033[0m \033[31mdecremented\033[0m to %d (for machine %d)\n",
				   worker_lid, credits[VAL_UD_QP_ID][j], j);
		}


		if(ops[i].state == ST_PUT_COMPLETE_SEND_VALS)
			ops[i].state = ST_PUT_COMPLETE;
		else
			ops[i].state = ST_NEW;

		// Create the broadcast messages
		forge_bcast_val_wrs(&ops[i], &val_send_packet_ops[*val_push_ptr], send_val_wr,
							send_val_sgl, cb, br_tx, br_i, worker_lid);
		total_vals_in_batch++;

		if(val_send_packet_ops[*val_push_ptr].req_num == VAL_MAX_REQ_COALESCE) {
			br_i++;
			if (br_i == MAX_PCIE_BCAST_BATCH) {
				batch_vals_2_NIC(send_val_wr, send_val_sgl, cb, br_i,
								 total_vals_in_batch, worker_lid);
				br_i = 0;
				total_vals_in_batch = 0;
			}
			HRD_MOD_ADD(*val_push_ptr, VAL_SEND_OPS_SIZE / REMOTE_MACHINES * group_membership.num_of_alive_remotes); //got to the next "packet" + dealing with failutes
			//Reset data left from previous bcasts after ibv_post_send to avoid sending reseted data
			val_send_packet_ops[*val_push_ptr].req_num = 0;
			for(j = 0; j < VAL_MAX_REQ_COALESCE; j++)
				val_send_packet_ops[*val_push_ptr].reqs[j].opcode = ST_EMPTY;
		}
	}

	if(val_send_packet_ops[*val_push_ptr].req_num > 0 &&
	   val_send_packet_ops[*val_push_ptr].req_num < VAL_MAX_REQ_COALESCE)
		br_i++;

	if(br_i > 0)
		batch_vals_2_NIC(send_val_wr, send_val_sgl, cb, br_i,
						 total_vals_in_batch, worker_lid);

	//Reset data left from previous bcasts
	if(val_send_packet_ops[*val_push_ptr].req_num > 0 &&
	   val_send_packet_ops[*val_push_ptr].req_num < VAL_MAX_REQ_COALESCE) {
		HRD_MOD_ADD(*val_push_ptr, VAL_SEND_OPS_SIZE / REMOTE_MACHINES * group_membership.num_of_alive_remotes); //got to the next "packet" + dealing with failutes
		val_send_packet_ops[*val_push_ptr].req_num = 0;
		for(j = 0; j < VAL_MAX_REQ_COALESCE; j++)
			val_send_packet_ops[*val_push_ptr].reqs[j].opcode = ST_EMPTY;
	}
	return has_outstanding_vals;
}

/* ---------------------------------------------------------------------------
------------------------------------CRDs--------------------------------------
---------------------------------------------------------------------------*/
static inline void
forge_crd_wr(uint16_t dst_node, struct ibv_send_wr* send_crd_wr,
			 struct hrd_ctrl_blk* cb, long long* send_crd_tx,
			 uint16_t send_crd_packets, uint8_t crd_to_send, uint16_t worker_lid)
{
	struct ibv_wc signal_send_wc;
	uint16_t dst_worker_gid = (uint16_t) (dst_node * WORKERS_PER_MACHINE + worker_lid);

	((spacetime_crd_t*) &send_crd_wr[send_crd_packets].imm_data)->val_credits = crd_to_send;

	send_crd_wr[send_crd_packets].wr.ud.ah = remote_worker_qps[dst_worker_gid][CRD_UD_QP_ID].ah;
	send_crd_wr[send_crd_packets].wr.ud.remote_qpn = (uint32) remote_worker_qps[dst_worker_gid][CRD_UD_QP_ID].qpn;
	send_crd_wr[send_crd_packets].send_flags = IBV_SEND_INLINE;

	w_stats[worker_lid].issued_crds_per_worker += crd_to_send;
	if (send_crd_packets > 0)
		send_crd_wr[send_crd_packets - 1].next = &send_crd_wr[send_crd_packets];

	if (*send_crd_tx % CRD_SS_GRANULARITY == 0) {
		if (*send_crd_tx > 0) {
			hrd_poll_cq(cb->dgram_send_cq[CRD_UD_QP_ID], 1, &signal_send_wc);
			w_stats[worker_lid].crd_ss_completions_per_worker++;
			if (ENABLE_SS_PRINTS && ENABLE_CRD_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
				red_printf("^^^ Polled SS completion[W%d]: \033[1m\033[36mCRD\033[0m %d (total %d)\n", worker_lid,
						   1, w_stats[worker_lid].crd_ss_completions_per_worker);
		}
		send_crd_wr[send_crd_packets].send_flags |= IBV_SEND_SIGNALED;
		if (ENABLE_SS_PRINTS && ENABLE_CRD_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
			red_printf("vvv Send SS[W%d]: \033[1m\033[36mCRD\033[0m\n", worker_lid);
	}
	(*send_crd_tx)++; // Selective signaling
}

static inline void
batch_crds_2_NIC(struct ibv_send_wr *send_crd_wr, struct hrd_ctrl_blk *cb,
				 uint16_t send_crd_packets, uint16_t total_crds_to_send,
				 uint16_t worker_lid)
{
	int ret, j = 0;
	struct ibv_send_wr *bad_send_wr;

	w_stats[worker_lid].issued_packet_crds_per_worker += send_crd_packets;
	send_crd_wr[send_crd_packets - 1].next = NULL;
	if(ENABLE_ASSERTIONS){
		for(j = 0; j < send_crd_packets - 1; j++){
			assert(send_crd_wr[j].next == &send_crd_wr[j+1]);
			assert(send_crd_wr[j].opcode == IBV_WR_SEND_WITH_IMM);
			assert(send_crd_wr[j].wr.ud.remote_qkey == HRD_DEFAULT_QKEY);
			assert(send_crd_wr[j].sg_list->length == 0);
			assert(send_crd_wr[j].num_sge == 0);
			assert(send_crd_wr[j].send_flags == IBV_SEND_INLINE ||
				   send_crd_wr[j].send_flags == IBV_SEND_INLINE | IBV_SEND_SIGNALED);
			assert(((spacetime_crd_t*) &(send_crd_wr[j].imm_data))->val_credits > 0);
			assert(((spacetime_crd_t*) &(send_crd_wr[j].imm_data))->opcode == ST_OP_CRD);
			assert(((spacetime_crd_t*) &(send_crd_wr[j].imm_data))->sender == machine_id);
		}
		assert(send_crd_wr[j].next == NULL);
	}

	if(ENABLE_SEND_PRINTS && ENABLE_CRD_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
		cyan_printf(">>> Send[W%d]: %d packets \033[1m\033[36mCRDs\033[0m %d (Total packets: %d, credits: %d)\n",
					worker_lid, send_crd_packets, total_crds_to_send, w_stats[worker_lid].issued_crds_per_worker,
					w_stats[worker_lid].issued_packet_crds_per_worker);
	ret = ibv_post_send(cb->dgram_qp[CRD_UD_QP_ID], &send_crd_wr[0], &bad_send_wr);
	CPE(ret, "ibv_post_send error while sending CRDs", ret);
}

static inline void
issue_credits(spacetime_val_t *val_recv_ops, long long int* send_crd_tx,
			  struct ibv_send_wr *send_crd_wr, uint8_t credits[][MACHINE_NUM],
			  struct hrd_ctrl_blk *cb, uint16_t worker_lid)
{
	uint16_t i = 0, send_crd_packets = 0;
	uint8_t credits_per_machine[MACHINE_NUM] = {0}, total_credits_to_send = 0;
	for (i = 0; i < VAL_RECV_OPS_SIZE; i++) {
		if (val_recv_ops[i].opcode == ST_EMPTY)
			break;

		if(credits_per_machine[val_recv_ops[i].sender] == 0 &&
		   credits[CRD_UD_QP_ID][val_recv_ops[i].sender] == 0)
			assert(0); //we should always have credits for CRDs

//		*credit_debug_cnt = 0;

		if(ENABLE_ASSERTIONS)
			assert(credits_per_machine[val_recv_ops[i].sender] < 255);

		credits[CRD_UD_QP_ID][val_recv_ops[i].sender]--;

		if (ENABLE_CREDIT_PRINTS && ENABLE_CRD_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
			printf("$$$ Credits[W%d]: \033[1m\033[36mCRDs\033[0m \033[31mdecremented\033[0m to %d (for machine %d)\n",
				   worker_lid, credits[CRD_UD_QP_ID][val_recv_ops[i].sender], val_recv_ops[i].sender);


		credits_per_machine[val_recv_ops[i].sender]++;
		//empty inv buffer
		if(val_recv_ops[i].opcode == ST_VAL_SUCCESS)
			val_recv_ops[i].opcode = ST_EMPTY;
		else assert(0);
	}
	for(i = 0; i < MACHINE_NUM; i ++){
		if(i == machine_id) continue;
		if(credits_per_machine[i] > 0) {
			forge_crd_wr(i, send_crd_wr, cb, send_crd_tx, send_crd_packets, credits_per_machine[i], worker_lid);
			send_crd_packets++;
			total_credits_to_send += credits_per_machine[i];
			if (send_crd_packets == MAX_SEND_CRD_WRS) {
				batch_crds_2_NIC(send_crd_wr, cb, send_crd_packets, total_credits_to_send, worker_lid);
				send_crd_packets = 0;
				total_credits_to_send = 0;
			}
		}
	}

	if (send_crd_packets > 0)
		batch_crds_2_NIC(send_crd_wr, cb, send_crd_packets, total_credits_to_send, worker_lid);

	if(ENABLE_ASSERTIONS )
		for(i = 0; i < VAL_RECV_OPS_SIZE; i++)
			assert(val_recv_ops[i].opcode == ST_EMPTY);
}

/* ---------------------------------------------------------------------------
----------------------------------- MEMBERSHIP -------------------------------
---------------------------------------------------------------------------*/
static inline void
follower_removal(int failed_node_id)
{
	red_printf("Failed node %d!\n", failed_node_id);
    if(ENABLE_ASSERTIONS == 1){
		assert(failed_node_id != machine_id);
		assert(failed_node_id < MACHINE_NUM);
	}
	optik_lock_membership(&group_membership.optik_lock);
    group_membership.write_ack_init  [failed_node_id / 8] |= (uint8_t) 1 << failed_node_id % 8;
	group_membership.group_membership[failed_node_id / 8] &= ~((uint8_t) 1 << failed_node_id % 8);
	group_membership.num_of_alive_remotes--;
//	green_printf("Group mem. bit array: "BYTE_TO_BINARY_PATTERN" \n",
//					   BYTE_TO_BINARY(group_membership.group_membership[0]));
	optik_unlock_membership(&group_membership.optik_lock);

	red_printf("Group membership changed --> Removed follower %d!\n", failed_node_id);

	if(group_membership.num_of_alive_remotes < (MACHINE_NUM / 2)){
		red_printf("Majority is down!\n");
		exit(-1);
	}
}

static inline uint8_t
group_membership_has_changed(spacetime_group_membership* last_group_membership,
							 uint16_t worker_lid)
{
	int i;
	uint32_t debug_lock_free_membership_read_cntr = 0;
	spacetime_group_membership lock_free_read_group_membership;
	do { //Lock free read of group membership
		if (ENABLE_ASSERTIONS) {
			debug_lock_free_membership_read_cntr++;
			if (debug_lock_free_membership_read_cntr == M_4) {
				printf("Worker %u stuck on a lock-free read (for group membership)\n", worker_lid);
				debug_lock_free_membership_read_cntr = 0;
			}
		}
		lock_free_read_group_membership = *((spacetime_group_membership*) &group_membership);
	} while (!(group_mem_timestamp_is_same_and_valid(group_membership.optik_lock.version,
													 lock_free_read_group_membership.optik_lock.version)));
	for(i = 0; i < GROUP_MEMBERSHIP_ARRAY_SIZE; i++)
		if(lock_free_read_group_membership.group_membership[i] != last_group_membership->group_membership[i]){
			*last_group_membership = lock_free_read_group_membership;
			return 1;
		}
	return 0;
}

static inline uint8_t
node_is_in_membership(spacetime_group_membership last_group_membership, int node_id)
{
	if((last_group_membership.group_membership[node_id / 8] >> (node_id % 8)) % 2 == 1)
		return 1;
	return 0;
}

/* ---------------------------------------------------------------------------
------------------------------------OTHERS------------------------------------
---------------------------------------------------------------------------*/
static inline void
refill_ops(uint32_t* trace_iter, uint16_t worker_lid,
		   struct spacetime_trace_command *trace, spacetime_op_t *ops,
		   int* stuck_op_index, uint32_t* refilled_per_ops_debug_cnt)
{
	static uint8_t first_iter_has_passed[WORKERS_PER_MACHINE] = { 0 };
	int i = 0, refilled_ops = 0;
	for(i = 0; i < MAX_BATCH_OPS_SIZE; i++) {
		if(ENABLE_ASSERTIONS){
			if(first_iter_has_passed[worker_lid] == 1){
				assert(ops[i].opcode == ST_OP_PUT || ops[i].opcode == ST_OP_GET);
				assert(ops[i].state == ST_PUT_COMPLETE ||
					   ops[i].state == ST_GET_COMPLETE ||
					   ops[i].state == ST_PUT_SUCCESS ||
					   ops[i].state == ST_REPLAY_SUCCESS ||
					   ops[i].state == ST_PUT_STALL ||
					   ops[i].state == ST_REPLAY_COMPLETE ||
					   ops[i].state == ST_IN_PROGRESS_PUT ||
					   ops[i].state == ST_IN_PROGRESS_REPLAY ||
					   ops[i].state == ST_PUT_COMPLETE_SEND_VALS ||
					   ops[i].state == ST_GET_STALL);
			}
		}
		if (first_iter_has_passed[worker_lid] == 0 ||
			ops[i].state == ST_PUT_COMPLETE || ops[i].state == ST_GET_COMPLETE) {
			if(first_iter_has_passed[worker_lid] != 0) {
				if (ENABLE_REQ_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
					green_printf("W%d--> Key Hash:%" PRIu64 "\n\t\tType: %s, version %d, tie-b: %d, value(len-%d): %c\n",
								 worker_lid, ((uint64_t *) &ops[i].key)[0],
								 code_to_str(ops[i].state), ops[i].version,
								 ops[i].tie_breaker_id, ops[i].val_len, ops[i].value[0]);
				ops[i].state = ST_EMPTY;
				ops[i].opcode = ST_EMPTY;
				w_stats[worker_lid].completed_ops_per_worker++;
				refilled_per_ops_debug_cnt[i] = 0;
				refilled_ops++;
			}
			if(ENABLE_ASSERTIONS)
				assert(trace[*trace_iter].opcode == ST_OP_PUT || trace[*trace_iter].opcode == ST_OP_GET);

			ops[i].state = ST_NEW;
			ops[i].opcode = trace[*trace_iter].opcode;
			memcpy(&ops[i].key, &trace[*trace_iter].key_hash, sizeof(spacetime_key_t));

			if (ops[i].opcode == ST_OP_PUT)
				memset(ops[i].value, ((uint8_t) 'x' + machine_id), ST_VALUE_SIZE);
			ops[i].val_len = (uint8) (ops[i].opcode == ST_OP_PUT ? ST_VALUE_SIZE : 0);
			if(ENABLE_REQ_PRINTS &&  worker_lid < MAX_THREADS_TO_PRINT)
				red_printf("W%d--> Op: %s, hash(1st 8B):%" PRIu64 "\n",
						   worker_lid, code_to_str(ops[i].opcode), ((uint64_t *) &ops[i].key)[0]);
			HRD_MOD_ADD(*trace_iter, TRACE_SIZE);
		}else if(ops[i].state == ST_IN_PROGRESS_PUT || ops[i].state == ST_IN_PROGRESS_REPLAY){
			refilled_per_ops_debug_cnt[i]++;
            if(unlikely(refilled_per_ops_debug_cnt[i] > M_1)){
				*stuck_op_index = i;
                refilled_per_ops_debug_cnt[i] = 0;
			}
		}
	}

	if(refilled_ops == 0)
		w_stats[worker_lid].wasted_loops++;

	if(first_iter_has_passed[worker_lid] == 0)
		first_iter_has_passed[worker_lid] = 1;

	if(ENABLE_ASSERTIONS)
		for(i = 0; i < MAX_BATCH_OPS_SIZE; i++)
			assert(ops[i].opcode == ST_OP_PUT || ops[i].opcode == ST_OP_GET);
}
static inline void
emulating_failre_detection(int* stuck_op_index, spacetime_op_t* ops, spacetime_group_membership last_group_membership,
						   uint32_t* num_of_iters_serving_op, uint16_t worker_lid){
    int node_suspected, i;
	node_suspected = find_failed_node(&ops[*stuck_op_index], worker_lid, last_group_membership);
	yellow_printf("W[%d] Refill_ops is stuck--> node suspicion: %d!\n", worker_lid, node_suspected);
	*stuck_op_index = -1;
	if (node_suspected >= 0) {
		if(ENABLE_ASSERTIONS) {
			assert(node_suspected < MACHINE_NUM);
			assert(node_suspected != machine_id);
			assert(node_is_in_membership(last_group_membership, node_suspected));
		}
		node_suspicions[worker_lid][node_suspected] = 1;
		//uncomment this if you want to increase certenty before removing a node
//				threads_suspecting_this_node = 0;
//				for(i = 0; i < WORKERS_PER_MACHINE; i++){
//					if(node_suspicions[i][node_suspected] == 0)
//						threads_suspecting_this_node++;
//				}
//				if(worker_lid == 0 && (threads_suspecting_this_node == WORKERS_PER_MACHINE ||
//									  threads_suspecting_this_node > 7)){
		if(worker_lid == 0){
			yellow_printf("W[%d] Refill_ops is stuck--> node suspicion: %d!\n", worker_lid, node_suspected);
			follower_removal(node_suspected);
		}

		for(i = 0; i < MAX_BATCH_OPS_SIZE; i++)
			num_of_iters_serving_op[i] = 0;
	}
}
#endif //HERMES_INLINE_UTIL_H
