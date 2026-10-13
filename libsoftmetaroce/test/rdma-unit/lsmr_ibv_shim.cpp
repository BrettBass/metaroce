/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <infiniband/verbs.h>
#include <sys/mman.h>

#include <libsoftmetaroce/MetaRoceVerbs.h>

extern "C" void* __real_mmap(
    void* addr,
    size_t length,
    int prot,
    int flags,
    int fd,
    off_t offset);

// No device reads these pages: lsmr copies payloads with the processor.
extern "C" void* __wrap_mmap(
    void* addr,
    size_t length,
    int prot,
    int flags,
    int fd,
    off_t offset) {
  return __real_mmap(addr, length, prot, flags & ~MAP_LOCKED, fd, offset);
}

namespace {

libsoftmetaroce::MetaRoceVerbs& shimVerbs() {
  // Leaked on purpose: the emulation's receiver/sender threads must not race
  // process exit during teardown, and it lives for the whole run.
  static auto* verbs = new libsoftmetaroce::MetaRoceVerbs();
  return *verbs;
}

} // namespace

// Datapath verbs are static inline (they dispatch via context->ops), so they
// cannot be --wrapped; these trampolines go in the ops table instead.
extern "C" {

static int lsmrOpsPostSend(ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr** bad) {
  return shimVerbs().ibv_post_send(qp, wr, bad);
}

static int lsmrOpsPostRecv(ibv_qp* qp, ibv_recv_wr* wr, ibv_recv_wr** bad) {
  return shimVerbs().ibv_post_recv(qp, wr, bad);
}

static int
lsmrOpsPostSrqRecv(ibv_srq* srq, ibv_recv_wr* wr, ibv_recv_wr** bad) {
  return shimVerbs().ibv_post_srq_recv(srq, wr, bad);
}

static int lsmrOpsPollCq(ibv_cq* cq, int num_entries, ibv_wc* wc) {
  return shimVerbs().ibv_poll_cq(cq, num_entries, wc);
}

static int lsmrOpsReqNotifyCq(ibv_cq* /*cq*/, int /*solicited_only*/) {
  // Poll-only emulation: no comp channel, so arming is a no-op.
  return 0;
}

// Control-path verbs: real exported symbols, wrapped directly.

struct ibv_device** __wrap_ibv_get_device_list(int* num_devices) {
  // lsmr's ibv_get_device_list dereferences its argument unconditionally, so
  // give it a real pointer and hand the caller's optional out-param back only
  // when it was provided.
  int deviceCount = 0;
  ibv_device** list = shimVerbs().ibv_get_device_list(&deviceCount);
  if (num_devices) {
    *num_devices = deviceCount;
  }
  return list;
}

void __wrap_ibv_free_device_list(struct ibv_device** list) {
  shimVerbs().ibv_free_device_list(list);
}

struct ibv_context* __wrap_ibv_open_device(struct ibv_device* device) {
  ibv_context* ctx = shimVerbs().ibv_open_device(device);
  if (ctx) {
    ctx->ops.post_send = lsmrOpsPostSend;
    ctx->ops.post_recv = lsmrOpsPostRecv;
    ctx->ops.post_srq_recv = lsmrOpsPostSrqRecv;
    ctx->ops.poll_cq = lsmrOpsPollCq;
    ctx->ops.req_notify_cq = lsmrOpsReqNotifyCq;
    // Force ibv_query_port down its legacy path (the real symbol we wrap)
    // instead of the unwrappable verbs_context::query_port fast path.
    ctx->abi_compat = nullptr;
  }
  return ctx;
}

int __wrap_ibv_close_device(struct ibv_context* context) {
  return shimVerbs().ibv_close_device(context);
}

int __wrap_ibv_query_device(
    struct ibv_context* context,
    struct ibv_device_attr* device_attr) {
  return shimVerbs().ibv_query_device(context, device_attr);
}

int __wrap_ibv_query_port(
    struct ibv_context* context,
    uint8_t port_num,
    struct ibv_port_attr* port_attr) {
  return shimVerbs().ibv_query_port(context, port_num, port_attr);
}

int __wrap_ibv_query_gid(
    struct ibv_context* context,
    uint8_t port_num,
    int index,
    union ibv_gid* gid) {
  return shimVerbs().ibv_query_gid(context, port_num, index, gid);
}

struct ibv_pd* __wrap_ibv_alloc_pd(struct ibv_context* context) {
  ibv_pd* pd = shimVerbs().ibv_alloc_pd(context);
  if (pd) {
    // Anchor pd->context to the ops-populated context so QPs/CQs created from
    // it inherit the datapath ops table.
    pd->context = context;
  }
  return pd;
}

int __wrap_ibv_dealloc_pd(struct ibv_pd* pd) {
  return shimVerbs().ibv_dealloc_pd(pd);
}

struct ibv_mr*
__wrap_ibv_reg_mr(struct ibv_pd* pd, void* addr, size_t length, int access) {
  return shimVerbs().ibv_reg_mr_internal(pd, addr, length, access);
}

// ibv_reg_mr is a macro that may expand to ibv_reg_mr_iova2 (or
// ibv_reg_mr_iova) depending on the access flags; wrap those underlying symbols
// too.
struct ibv_mr* __wrap_ibv_reg_mr_iova2(
    struct ibv_pd* pd,
    void* addr,
    size_t length,
    uint64_t /*iova*/,
    unsigned int access) {
  return shimVerbs().ibv_reg_mr_internal(pd, addr, length, (int)access);
}

struct ibv_mr* __wrap_ibv_reg_mr_iova(
    struct ibv_pd* pd,
    void* addr,
    size_t length,
    uint64_t /*iova*/,
    int access) {
  return shimVerbs().ibv_reg_mr_internal(pd, addr, length, access);
}

int __wrap_ibv_dereg_mr(struct ibv_mr* mr) {
  return shimVerbs().ibv_dereg_mr(mr);
}

struct ibv_comp_channel* __wrap_ibv_create_comp_channel(
    struct ibv_context* context) {
  return shimVerbs().ibv_create_comp_channel(context);
}

int __wrap_ibv_destroy_comp_channel(struct ibv_comp_channel* channel) {
  return shimVerbs().ibv_destroy_comp_channel(channel);
}

struct ibv_cq* __wrap_ibv_create_cq(
    struct ibv_context* context,
    int cqe,
    void* cq_context,
    struct ibv_comp_channel* channel,
    int comp_vector) {
  ibv_cq* cq =
      shimVerbs().ibv_create_cq(context, cqe, cq_context, channel, comp_vector);
  if (cq) {
    cq->context = context;
  }
  return cq;
}

int __wrap_ibv_destroy_cq(struct ibv_cq* cq) {
  return shimVerbs().ibv_destroy_cq(cq);
}

struct ibv_qp* __wrap_ibv_create_qp(
    struct ibv_pd* pd,
    struct ibv_qp_init_attr* qp_init_attr) {
  ibv_qp* qp = shimVerbs().ibv_create_qp(pd, qp_init_attr);
  if (qp) {
    qp->context = pd->context;
  }
  return qp;
}

int __wrap_ibv_destroy_qp(struct ibv_qp* qp) {
  return shimVerbs().ibv_destroy_qp(qp);
}

int __wrap_ibv_modify_qp(
    struct ibv_qp* qp,
    struct ibv_qp_attr* attr,
    int attr_mask) {
  return shimVerbs().ibv_modify_qp(qp, attr, attr_mask);
}

int __wrap_ibv_query_qp(
    struct ibv_qp* qp,
    struct ibv_qp_attr* attr,
    int attr_mask,
    struct ibv_qp_init_attr* init_attr) {
  return shimVerbs().ibv_query_qp(qp, attr, attr_mask, init_attr);
}

struct ibv_srq* __wrap_ibv_create_srq(
    struct ibv_pd* pd,
    struct ibv_srq_init_attr* srq_init_attr) {
  return shimVerbs().ibv_create_srq(pd, srq_init_attr);
}

int __wrap_ibv_modify_srq(
    struct ibv_srq* srq,
    struct ibv_srq_attr* srq_attr,
    int srq_attr_mask) {
  return shimVerbs().ibv_modify_srq(srq, srq_attr, srq_attr_mask);
}

int __wrap_ibv_query_srq(struct ibv_srq* srq, struct ibv_srq_attr* srq_attr) {
  return shimVerbs().ibv_query_srq(srq, srq_attr);
}

int __wrap_ibv_destroy_srq(struct ibv_srq* srq) {
  return shimVerbs().ibv_destroy_srq(srq);
}

int __wrap_ibv_get_async_event(
    struct ibv_context* context,
    struct ibv_async_event* event) {
  return shimVerbs().ibv_get_async_event(context, event);
}

void __wrap_ibv_ack_async_event(struct ibv_async_event* event) {
  shimVerbs().ibv_ack_async_event(event);
}

} // extern "C"
