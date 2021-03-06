/*
 * Copyright (c) 2018 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *	   Redistribution and use in source and binary forms, with or
 *	   without modification, are permitted provided that the following
 *	   conditions are met:
 *
 *		- Redistributions of source code must retain the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer.
 *
 *		- Redistributions in binary form must reproduce the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer in the documentation and/or other materials
 *		  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <rdma/fi_errno.h>
#include "rdma/fi_eq.h"
#include "ofi_iov.h"
#include <ofi_prov.h>
#include "tcpx.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <ofi_util.h>
#include <unistd.h>
#include <string.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netdb.h>

static inline struct tcpx_xfer_entry *
tcpx_alloc_recv_entry(struct tcpx_ep *tcpx_ep)
{
	struct tcpx_xfer_entry *recv_entry;
	struct tcpx_cq *tcpx_cq;

	tcpx_cq = container_of(tcpx_ep->util_ep.rx_cq, struct tcpx_cq, util_cq);

	recv_entry = tcpx_xfer_entry_alloc(tcpx_cq, TCPX_OP_MSG_RECV);
	if (recv_entry)
		recv_entry->ep = tcpx_ep;

	return recv_entry;
}

static inline struct tcpx_xfer_entry *
tcpx_alloc_send_entry(struct tcpx_ep *tcpx_ep)
{
	struct tcpx_xfer_entry *send_entry;
	struct tcpx_cq *tcpx_cq;

	tcpx_cq = container_of(tcpx_ep->util_ep.tx_cq, struct tcpx_cq, util_cq);

	send_entry = tcpx_xfer_entry_alloc(tcpx_cq, TCPX_OP_MSG_SEND);
	if (send_entry)
		send_entry->ep = tcpx_ep;

	return send_entry;
}

static inline void tcpx_queue_recv(struct tcpx_ep *tcpx_ep,
				   struct tcpx_xfer_entry *recv_entry)
{
	fastlock_acquire(&tcpx_ep->lock);
	slist_insert_tail(&recv_entry->entry, &tcpx_ep->rx_queue);
	fastlock_release(&tcpx_ep->lock);
}

static ssize_t tcpx_recvmsg(struct fid_ep *ep, const struct fi_msg *msg,
			    uint64_t flags)
{
	struct tcpx_xfer_entry *recv_entry;
	struct tcpx_ep *tcpx_ep;

	tcpx_ep = container_of(ep, struct tcpx_ep, util_ep.ep_fid);

	assert(msg->iov_count <= TCPX_IOV_LIMIT);

	recv_entry = tcpx_alloc_recv_entry(tcpx_ep);
	if (!recv_entry)
		return -FI_EAGAIN;

	recv_entry->iov_cnt = msg->iov_count;
	memcpy(&recv_entry->iov[0], &msg->msg_iov[0],
	       msg->iov_count * sizeof(struct iovec));

	recv_entry->flags = tcpx_ep->util_ep.rx_msg_flags | flags |
			    FI_MSG | FI_RECV;
	recv_entry->context = msg->context;

	tcpx_queue_recv(tcpx_ep, recv_entry);
	return FI_SUCCESS;
}

static ssize_t tcpx_recv(struct fid_ep *ep, void *buf, size_t len, void *desc,
			 fi_addr_t src_addr, void *context)
{
	struct tcpx_xfer_entry *recv_entry;
	struct tcpx_ep *tcpx_ep;

	tcpx_ep = container_of(ep, struct tcpx_ep, util_ep.ep_fid);

	recv_entry = tcpx_alloc_recv_entry(tcpx_ep);
	if (!recv_entry)
		return -FI_EAGAIN;

	recv_entry->iov_cnt = 1;
	recv_entry->iov[0].iov_base = buf;
	recv_entry->iov[0].iov_len = len;

	recv_entry->flags = (tcpx_ep->util_ep.rx_op_flags & FI_COMPLETION) |
			    FI_MSG | FI_RECV;
	recv_entry->context = context;

	tcpx_queue_recv(tcpx_ep, recv_entry);
	return FI_SUCCESS;
}

static ssize_t tcpx_recvv(struct fid_ep *ep, const struct iovec *iov, void **desc,
			  size_t count, fi_addr_t src_addr, void *context)
{
	struct tcpx_xfer_entry *recv_entry;
	struct tcpx_ep *tcpx_ep;

	tcpx_ep = container_of(ep, struct tcpx_ep, util_ep.ep_fid);

	assert(count <= TCPX_IOV_LIMIT);

	recv_entry = tcpx_alloc_recv_entry(tcpx_ep);
	if (!recv_entry)
		return -FI_EAGAIN;

	recv_entry->iov_cnt = count;
	memcpy(recv_entry->iov, iov, count * sizeof(*iov));

	recv_entry->flags = (tcpx_ep->util_ep.rx_op_flags & FI_COMPLETION) |
			    FI_MSG | FI_RECV;
	recv_entry->context = context;

	tcpx_queue_recv(tcpx_ep, recv_entry);
	return FI_SUCCESS;
}

static inline void tcpx_queue_send(struct tcpx_ep *ep,
				   struct tcpx_xfer_entry *tx_entry)
{
	tx_entry->rem_len = tx_entry->hdr.base_hdr.size;
	ep->hdr_bswap(&tx_entry->hdr.base_hdr);

	fastlock_acquire(&ep->lock);
	tcpx_tx_queue_insert(ep, tx_entry);
	fastlock_release(&ep->lock);
}

static ssize_t tcpx_sendmsg(struct fid_ep *ep, const struct fi_msg *msg,
			    uint64_t flags)
{
	struct tcpx_ep *tcpx_ep;
	struct tcpx_xfer_entry *tx_entry;
	uint64_t data_len;
	size_t offset;

	tcpx_ep = container_of(ep, struct tcpx_ep, util_ep.ep_fid);
	tx_entry = tcpx_alloc_send_entry(tcpx_ep);
	if (!tx_entry)
		return -FI_EAGAIN;

	assert(msg->iov_count <= TCPX_IOV_LIMIT);
	data_len = ofi_total_iov_len(msg->msg_iov, msg->iov_count);
	assert(!(flags & FI_INJECT) || (data_len <= TCPX_MAX_INJECT));

	if (flags & FI_REMOTE_CQ_DATA) {
		tx_entry->hdr.base_hdr.flags = TCPX_REMOTE_CQ_DATA;
		tx_entry->hdr.cq_data_hdr.cq_data = msg->data;
		offset = sizeof(tx_entry->hdr.cq_data_hdr);
	} else {
		offset = sizeof(tx_entry->hdr.base_hdr);
	}

	tx_entry->hdr.base_hdr.payload_off = (uint8_t) offset;
	tx_entry->hdr.base_hdr.size = offset + data_len;
	if (flags & FI_INJECT) {
		ofi_copy_iov_buf(msg->msg_iov, msg->iov_count, 0,
				 (uint8_t *) &tx_entry->hdr + offset,
				 data_len,
				 OFI_COPY_IOV_TO_BUF);
		tx_entry->iov_cnt = 1;
		offset += data_len;
	} else {
		memcpy(&tx_entry->iov[1], &msg->msg_iov[0],
		       msg->iov_count * sizeof(struct iovec));

		tx_entry->iov_cnt = msg->iov_count + 1;
	}
	tx_entry->iov[0].iov_base = (void *) &tx_entry->hdr;
	tx_entry->iov[0].iov_len = offset;

	tx_entry->flags = ((tcpx_ep->util_ep.tx_op_flags & FI_COMPLETION) |
			    flags | FI_MSG | FI_SEND);

	if (flags & (FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE))
		tx_entry->hdr.base_hdr.flags |= TCPX_DELIVERY_COMPLETE;

	tx_entry->context = msg->context;

	tcpx_queue_send(tcpx_ep, tx_entry);
	return FI_SUCCESS;
}

static ssize_t tcpx_send(struct fid_ep *ep, const void *buf, size_t len,
			 void *desc, fi_addr_t dest_addr, void *context)
{
	struct tcpx_ep *tcpx_ep;
	struct tcpx_xfer_entry *tx_entry;

	tcpx_ep = container_of(ep, struct tcpx_ep, util_ep.ep_fid);

	tx_entry = tcpx_alloc_send_entry(tcpx_ep);
	if (!tx_entry)
		return -FI_EAGAIN;

	tx_entry->hdr.base_hdr.size = len + sizeof(tx_entry->hdr.base_hdr);
	tx_entry->hdr.base_hdr.payload_off = (uint8_t)
					     sizeof(tx_entry->hdr.base_hdr);

	tx_entry->iov[0].iov_base = (void *) &tx_entry->hdr;
	tx_entry->iov[0].iov_len = sizeof(tx_entry->hdr.base_hdr);
	tx_entry->iov[1].iov_base = (void *) buf;
	tx_entry->iov[1].iov_len = len;
	tx_entry->iov_cnt = 2;

	tx_entry->context = context;
	tx_entry->flags = (tcpx_ep->util_ep.tx_op_flags & FI_COMPLETION) |
			   FI_MSG | FI_SEND;

	if (tcpx_ep->util_ep.tx_op_flags &
	    (FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE))
		tx_entry->hdr.base_hdr.flags = OFI_DELIVERY_COMPLETE;

	tcpx_queue_send(tcpx_ep, tx_entry);
	return FI_SUCCESS;
}

static ssize_t tcpx_sendv(struct fid_ep *ep, const struct iovec *iov,
			  void **desc, size_t count, fi_addr_t dest_addr,
			  void *context)
{
	struct tcpx_ep *tcpx_ep;
	struct tcpx_xfer_entry *tx_entry;
	uint64_t data_len;

	tcpx_ep = container_of(ep, struct tcpx_ep, util_ep.ep_fid);

	tx_entry = tcpx_alloc_send_entry(tcpx_ep);
	if (!tx_entry)
		return -FI_EAGAIN;

	assert(count <= TCPX_IOV_LIMIT);
	data_len = ofi_total_iov_len(iov, count);
	tx_entry->hdr.base_hdr.size = data_len + sizeof(tx_entry->hdr.base_hdr);
	tx_entry->hdr.base_hdr.payload_off = (uint8_t)
					     sizeof(tx_entry->hdr.base_hdr);

	tx_entry->iov[0].iov_base = (void *) &tx_entry->hdr;
	tx_entry->iov[0].iov_len = sizeof(tx_entry->hdr.base_hdr);
	tx_entry->iov_cnt = count + 1;
	memcpy(&tx_entry->iov[1], &iov[0], count * sizeof(*iov));

	tx_entry->context = context;
	tx_entry->flags = (tcpx_ep->util_ep.tx_op_flags & FI_COMPLETION) |
			   FI_MSG | FI_SEND;

	if (tcpx_ep->util_ep.tx_op_flags &
	    (FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE))
		tx_entry->hdr.base_hdr.flags = TCPX_DELIVERY_COMPLETE;

	tcpx_queue_send(tcpx_ep, tx_entry);
	return FI_SUCCESS;
}


static ssize_t tcpx_inject(struct fid_ep *ep, const void *buf, size_t len,
			   fi_addr_t dest_addr)
{
	struct tcpx_ep *tcpx_ep;
	struct tcpx_xfer_entry *tx_entry;
	size_t offset;

	tcpx_ep = container_of(ep, struct tcpx_ep, util_ep.ep_fid);

	tx_entry = tcpx_alloc_send_entry(tcpx_ep);
	if (!tx_entry)
		return -FI_EAGAIN;

	assert(len <= TCPX_MAX_INJECT);
	tx_entry->hdr.base_hdr.size = len + sizeof(tx_entry->hdr.base_hdr);

	offset = sizeof(tx_entry->hdr.base_hdr);
	tx_entry->hdr.base_hdr.payload_off = (uint8_t) offset;
	memcpy((uint8_t *)&tx_entry->hdr + offset, (uint8_t *) buf, len);

	tx_entry->iov[0].iov_base = (void *) &tx_entry->hdr;
	tx_entry->iov[0].iov_len = len + sizeof(tx_entry->hdr.base_hdr);
	tx_entry->iov_cnt = 1;
	tx_entry->flags = FI_MSG | FI_SEND;

	tcpx_queue_send(tcpx_ep, tx_entry);
	return FI_SUCCESS;
}

static ssize_t tcpx_senddata(struct fid_ep *ep, const void *buf, size_t len,
			     void *desc, uint64_t data, fi_addr_t dest_addr,
			     void *context)
{
	struct tcpx_ep *tcpx_ep;
	struct tcpx_xfer_entry *tx_entry;

	tcpx_ep = container_of(ep, struct tcpx_ep, util_ep.ep_fid);

	tx_entry = tcpx_alloc_send_entry(tcpx_ep);
	if (!tx_entry)
		return -FI_EAGAIN;

	tx_entry->hdr.cq_data_hdr.base_hdr.size =
		len + sizeof(tx_entry->hdr.cq_data_hdr);
	tx_entry->hdr.cq_data_hdr.base_hdr.flags = TCPX_REMOTE_CQ_DATA;

	tx_entry->hdr.cq_data_hdr.cq_data = data;

	tx_entry->hdr.cq_data_hdr.base_hdr.payload_off =
		(uint8_t) sizeof(tx_entry->hdr.cq_data_hdr);

	tx_entry->iov[0].iov_base = (void *) &tx_entry->hdr;
	tx_entry->iov[0].iov_len = sizeof(tx_entry->hdr.cq_data_hdr);
	tx_entry->iov[1].iov_base = (void *) buf;
	tx_entry->iov[1].iov_len = len;
	tx_entry->iov_cnt = 2;

	tx_entry->context = context;
	tx_entry->flags = (tcpx_ep->util_ep.tx_op_flags & FI_COMPLETION) |
			   FI_MSG | FI_SEND;

	if (tcpx_ep->util_ep.tx_op_flags &
	    (FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE))
		tx_entry->hdr.base_hdr.flags |= TCPX_DELIVERY_COMPLETE;

	tcpx_queue_send(tcpx_ep, tx_entry);
	return FI_SUCCESS;
}

static ssize_t tcpx_injectdata(struct fid_ep *ep, const void *buf, size_t len,
			       uint64_t data, fi_addr_t dest_addr)
{
	struct tcpx_ep *tcpx_ep;
	struct tcpx_xfer_entry *tx_entry;

	tcpx_ep = container_of(ep, struct tcpx_ep, util_ep.ep_fid);

	tx_entry = tcpx_alloc_send_entry(tcpx_ep);
	if (!tx_entry)
		return -FI_EAGAIN;

	assert(len <= TCPX_MAX_INJECT);

	tx_entry->hdr.cq_data_hdr.base_hdr.flags = TCPX_REMOTE_CQ_DATA;
	tx_entry->hdr.cq_data_hdr.cq_data = data;

	tx_entry->hdr.base_hdr.size = len + sizeof(tx_entry->hdr.cq_data_hdr);
	tx_entry->hdr.base_hdr.payload_off = (uint8_t)
					     sizeof(tx_entry->hdr.cq_data_hdr);

	memcpy((uint8_t *) &tx_entry->hdr + sizeof(tx_entry->hdr.cq_data_hdr),
	       (uint8_t *) buf, len);

	tx_entry->iov[0].iov_base = (void *) &tx_entry->hdr;
	tx_entry->iov[0].iov_len = len + sizeof(tx_entry->hdr.cq_data_hdr);
	tx_entry->iov_cnt = 1;
	tx_entry->flags = FI_MSG | FI_SEND;

	tcpx_queue_send(tcpx_ep, tx_entry);
	return FI_SUCCESS;
}

struct fi_ops_msg tcpx_msg_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = tcpx_recv,
	.recvv = tcpx_recvv,
	.recvmsg = tcpx_recvmsg,
	.send = tcpx_send,
	.sendv = tcpx_sendv,
	.sendmsg = tcpx_sendmsg,
	.inject = tcpx_inject,
	.senddata = tcpx_senddata,
	.injectdata = tcpx_injectdata,
};


/* There's no application driven need for tagged message operations over
 * connected endpoints.  The tcp provider exposes the ability to send
 * tagged messages using the tcp header, with the expectation that the
 * peer side is using dynamic receive buffers to match the tagged messages
 * with application buffers.  This provides an optimized path for rxm
 * over tcp, that allows rxm to drop its header in certain cases an only
 * use a minimal tcp header.
 */
static ssize_t
tcpx_tsendmsg(struct fid_ep *fid_ep, const struct fi_msg_tagged *msg,
	      uint64_t flags)
{
	struct tcpx_ep *ep;
	struct tcpx_xfer_entry *tx_entry;
	uint64_t data_len;
	size_t offset;

	ep = container_of(fid_ep, struct tcpx_ep, util_ep.ep_fid);
	tx_entry = tcpx_alloc_send_entry(ep);
	if (!tx_entry)
		return -FI_EAGAIN;

	assert(msg->iov_count <= TCPX_IOV_LIMIT);
	data_len = ofi_total_iov_len(msg->msg_iov, msg->iov_count);
	assert(!(flags & FI_INJECT) || (data_len <= TCPX_MAX_INJECT));

	if (flags & FI_REMOTE_CQ_DATA) {
		tx_entry->hdr.base_hdr.flags = TCPX_REMOTE_CQ_DATA |
						TCPX_TAGGED;
		tx_entry->hdr.tag_data_hdr.cq_data_hdr.cq_data = msg->data;
		tx_entry->hdr.tag_data_hdr.tag = msg->tag;
		offset = sizeof(tx_entry->hdr.tag_data_hdr);
	} else {
		tx_entry->hdr.base_hdr.flags = TCPX_TAGGED;
		tx_entry->hdr.tag_hdr.tag = msg->tag;
		offset = sizeof(tx_entry->hdr.tag_hdr);
	}

	tx_entry->hdr.base_hdr.payload_off = (uint8_t) offset;
	tx_entry->hdr.base_hdr.size = offset + data_len;

	if (flags & FI_INJECT) {
		ofi_copy_iov_buf(msg->msg_iov, msg->iov_count, 0,
				 (uint8_t *) &tx_entry->hdr + offset,
				 data_len, OFI_COPY_IOV_TO_BUF);
		tx_entry->iov_cnt = 1;
		offset += data_len;
	} else {
		memcpy(&tx_entry->iov[1], &msg->msg_iov[0],
		       msg->iov_count * sizeof(struct iovec));

		tx_entry->iov_cnt = msg->iov_count + 1;
	}
	tx_entry->iov[0].iov_base = (void *) &tx_entry->hdr;
	tx_entry->iov[0].iov_len = offset;

	tx_entry->flags = ((ep->util_ep.tx_op_flags & FI_COMPLETION) |
			    flags | FI_TAGGED | FI_SEND);

	if (flags & (FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE))
		tx_entry->hdr.base_hdr.flags |= TCPX_DELIVERY_COMPLETE;

	tx_entry->context = msg->context;

	tcpx_queue_send(ep, tx_entry);
	return FI_SUCCESS;
}

static ssize_t
tcpx_tsend(struct fid_ep *fid_ep, const void *buf, size_t len,
	   void *desc, fi_addr_t dest_addr, uint64_t tag, void *context)
{
	struct tcpx_ep *ep;
	struct tcpx_xfer_entry *tx_entry;

	ep = container_of(fid_ep, struct tcpx_ep, util_ep.ep_fid);
	tx_entry = tcpx_alloc_send_entry(ep);
	if (!tx_entry)
		return -FI_EAGAIN;

	tx_entry->hdr.base_hdr.flags = TCPX_TAGGED;
	tx_entry->hdr.tag_hdr.tag = tag;

	tx_entry->hdr.base_hdr.size = len + sizeof(tx_entry->hdr.tag_hdr);
	tx_entry->hdr.base_hdr.payload_off = (uint8_t)
					     sizeof(tx_entry->hdr.tag_hdr);

	tx_entry->iov[0].iov_base = (void *) &tx_entry->hdr;
	tx_entry->iov[0].iov_len = sizeof(tx_entry->hdr.tag_hdr);
	tx_entry->iov[1].iov_base = (void *) buf;
	tx_entry->iov[1].iov_len = len;
	tx_entry->iov_cnt = 2;

	tx_entry->context = context;
	tx_entry->flags = (ep->util_ep.tx_op_flags & FI_COMPLETION) |
			   FI_TAGGED | FI_SEND;

	if (ep->util_ep.tx_op_flags &
	    (FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE))
		tx_entry->hdr.base_hdr.flags |= OFI_DELIVERY_COMPLETE;

	tcpx_queue_send(ep, tx_entry);
	return FI_SUCCESS;
}

static ssize_t
tcpx_tsendv(struct fid_ep *fid_ep, const struct iovec *iov, void **desc,
	    size_t count, fi_addr_t dest_addr, uint64_t tag, void *context)
{
	struct tcpx_ep *ep;
	struct tcpx_xfer_entry *tx_entry;
	uint64_t data_len;

	ep = container_of(fid_ep, struct tcpx_ep, util_ep.ep_fid);
	tx_entry = tcpx_alloc_send_entry(ep);
	if (!tx_entry)
		return -FI_EAGAIN;

	assert(count <= TCPX_IOV_LIMIT);
	data_len = ofi_total_iov_len(iov, count);

	tx_entry->hdr.base_hdr.flags = TCPX_TAGGED;
	tx_entry->hdr.tag_hdr.tag = tag;

	tx_entry->hdr.base_hdr.size = data_len + sizeof(tx_entry->hdr.tag_hdr);
	tx_entry->hdr.base_hdr.payload_off = (uint8_t)
					     sizeof(tx_entry->hdr.tag_hdr);

	tx_entry->iov[0].iov_base = (void *) &tx_entry->hdr;
	tx_entry->iov[0].iov_len = sizeof(tx_entry->hdr.tag_hdr);
	tx_entry->iov_cnt = count + 1;
	memcpy(&tx_entry->iov[1], &iov[0], count * sizeof(struct iovec));

	tx_entry->context = context;
	tx_entry->flags = (ep->util_ep.tx_op_flags & FI_COMPLETION) |
			   FI_TAGGED | FI_SEND;

	if (ep->util_ep.tx_op_flags &
	    (FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE))
		tx_entry->hdr.base_hdr.flags |= TCPX_DELIVERY_COMPLETE;

	tcpx_queue_send(ep, tx_entry);
	return FI_SUCCESS;
}


static ssize_t
tcpx_tinject(struct fid_ep *fid_ep, const void *buf, size_t len,
	     fi_addr_t dest_addr, uint64_t tag)
{
	struct tcpx_ep *ep;
	struct tcpx_xfer_entry *tx_entry;

	ep = container_of(fid_ep, struct tcpx_ep, util_ep.ep_fid);
	tx_entry = tcpx_alloc_send_entry(ep);
	if (!tx_entry)
		return -FI_EAGAIN;

	assert(len <= TCPX_MAX_INJECT);
	tx_entry->hdr.base_hdr.flags = TCPX_TAGGED;
	tx_entry->hdr.tag_hdr.tag = tag;

	tx_entry->hdr.base_hdr.size = len + sizeof(tx_entry->hdr.tag_hdr);
	tx_entry->hdr.base_hdr.payload_off = (uint8_t)
					     sizeof(tx_entry->hdr.tag_hdr);

	memcpy((uint8_t *) &tx_entry->hdr + sizeof(tx_entry->hdr.tag_hdr),
	       (uint8_t *) buf, len);

	tx_entry->iov[0].iov_base = (void *) &tx_entry->hdr;
	tx_entry->iov[0].iov_len = len + sizeof(tx_entry->hdr.tag_hdr);
	tx_entry->iov_cnt = 1;
	tx_entry->flags = FI_TAGGED | FI_SEND;

	tcpx_queue_send(ep, tx_entry);
	return FI_SUCCESS;
}

static ssize_t
tcpx_tsenddata(struct fid_ep *fid_ep, const void *buf, size_t len, void *desc,
	       uint64_t data, fi_addr_t dest_addr, uint64_t tag, void *context)
{
	struct tcpx_ep *ep;
	struct tcpx_xfer_entry *tx_entry;

	ep = container_of(fid_ep, struct tcpx_ep, util_ep.ep_fid);
	tx_entry = tcpx_alloc_send_entry(ep);
	if (!tx_entry)
		return -FI_EAGAIN;

	tx_entry->hdr.base_hdr.flags = TCPX_TAGGED | TCPX_REMOTE_CQ_DATA;
	tx_entry->hdr.tag_data_hdr.tag = tag;
	tx_entry->hdr.tag_data_hdr.cq_data_hdr.cq_data = data;

	tx_entry->hdr.base_hdr.size = len + sizeof(tx_entry->hdr.tag_data_hdr);
	tx_entry->hdr.base_hdr.payload_off = (uint8_t)
					     sizeof(tx_entry->hdr.tag_data_hdr);

	tx_entry->iov[0].iov_base = (void *) &tx_entry->hdr;
	tx_entry->iov[0].iov_len = sizeof(tx_entry->hdr.tag_data_hdr);
	tx_entry->iov[1].iov_base = (void *) buf;
	tx_entry->iov[1].iov_len = len;
	tx_entry->iov_cnt = 2;

	tx_entry->context = context;
	tx_entry->flags = (ep->util_ep.tx_op_flags & FI_COMPLETION) |
			   FI_TAGGED | FI_SEND;

	if (ep->util_ep.tx_op_flags &
	    (FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE))
		tx_entry->hdr.base_hdr.flags |= TCPX_DELIVERY_COMPLETE;

	tcpx_queue_send(ep, tx_entry);
	return FI_SUCCESS;
}

static ssize_t
tcpx_tinjectdata(struct fid_ep *fid_ep, const void *buf, size_t len,
		 uint64_t data, fi_addr_t dest_addr, uint64_t tag)
{
	struct tcpx_ep *ep;
	struct tcpx_xfer_entry *tx_entry;

	ep = container_of(fid_ep, struct tcpx_ep, util_ep.ep_fid);

	tx_entry = tcpx_alloc_send_entry(ep);
	if (!tx_entry)
		return -FI_EAGAIN;

	assert(len <= TCPX_MAX_INJECT);

	tx_entry->hdr.base_hdr.flags = TCPX_TAGGED | TCPX_REMOTE_CQ_DATA;
	tx_entry->hdr.tag_data_hdr.tag = tag;
	tx_entry->hdr.tag_data_hdr.cq_data_hdr.cq_data = data;

	tx_entry->hdr.base_hdr.size = len + sizeof(tx_entry->hdr.tag_data_hdr);
	tx_entry->hdr.base_hdr.payload_off = (uint8_t)
					     sizeof(tx_entry->hdr.tag_data_hdr);

	memcpy((uint8_t *) &tx_entry->hdr + sizeof(tx_entry->hdr.tag_data_hdr),
	       (uint8_t *) buf, len);

	tx_entry->iov[0].iov_base = (void *) &tx_entry->hdr;
	tx_entry->iov[0].iov_len = len + sizeof(tx_entry->hdr.tag_data_hdr);
	tx_entry->iov_cnt = 1;
	tx_entry->flags = FI_TAGGED | FI_SEND;

	tcpx_queue_send(ep, tx_entry);
	return FI_SUCCESS;
}

struct fi_ops_tagged tcpx_tagged_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = fi_no_tagged_recv,
	.recvv = fi_no_tagged_recvv,
	.recvmsg = fi_no_tagged_recvmsg,
	.send = tcpx_tsend,
	.sendv = tcpx_tsendv,
	.sendmsg = tcpx_tsendmsg,
	.inject = tcpx_tinject,
	.senddata = tcpx_tsenddata,
	.injectdata = tcpx_tinjectdata,
};
