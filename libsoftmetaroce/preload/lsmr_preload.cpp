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

// LD_PRELOAD interposer presenting libsoftmetaroce as the verbs
// implementation to unmodified binaries (notably stock perftest). The
// loader binds the ibv_* symbols below to this library instead of
// libibverbs; each forwards to a process-wide MetaRoceVerbs instance, the
// same technique as the rdma-unit link-time shim in a shared-library form.
//
// Coverage is the ib_write_bw RC flow: every ibv_* that flow calls with a
// library-owned object is interposed here. Datapath verbs dispatch through
// context->ops, so open_device installs trampolines covering them, and the
// extended query, null-MR, and device-memory verbs either resolve through
// those interposed symbols or fail cleanly at the header inline. The direct
// interposers plus the trampolines serve the whole flow. Configuration
// arrives by environment, since a preloaded library cannot see the binary's
// argv:
//   LSMR_PRELOAD_GIDS  comma-separated local IPs to use as GIDs, the
//                      --libsoftmetaroce_gid value; unset leaves the
//                      backend's address auto-discovery in place.

#include <libsoftmetaroce/MetaRoceVerbs.h>

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <gflags/gflags.h>
#include <infiniband/verbs.h>

#include <folly/CppAttributes.h>
#include <folly/Singleton.h>
#include <folly/init/Init.h>
#include <folly/init/Phase.h>
#include <chrono>

// verbs.h defines some of these as macros; the interposer needs the real
// names. Undefining a plain function is a no-op.
#undef ibv_get_device_list
#undef ibv_free_device_list
#undef ibv_get_device_name
#undef ibv_open_device
#undef ibv_close_device
#undef ibv_query_device
#undef ibv_query_port
#undef ibv_query_gid
#undef ibv_query_gid_type
#undef ibv_alloc_pd
#undef ibv_dealloc_pd
#undef ibv_create_cq
#undef ibv_destroy_cq
#undef ibv_reg_mr
#undef ibv_reg_mr_iova
#undef ibv_reg_mr_iova2
#undef ibv_dereg_mr
#undef ibv_create_qp
#undef ibv_modify_qp
#undef ibv_query_qp
#undef ibv_destroy_qp
#undef ibv_create_comp_channel
#undef ibv_destroy_comp_channel

namespace {

// A preloaded C binary never calls folly::init, but MetaRoceVerbs
// needs the singleton registry; initialize it here, lazily, since
// the library constructor runs before main and folly forbids
// singleton use that early. Environment configuration applies here
// too, at first use rather than in a constructor: the constructor
// can run before the backend registers its flags, and setting an
// unregistered flag silently drops the value.
void ensureFollyInit() {
  // Every caller enters this initializer and waits for it: the phase flips
  // to Regular before singleton registration finishes, so checking it
  // outside the guard lets a second thread proceed mid-initialization.
  static bool initialized = [] {
    // A preloaded library cannot see the binary's argv, so configuration
    // arrives by environment. Apply it before anything reads the flags:
    // first use runs after all constructors, so the flags exist. It also
    // applies when a C++ host initialized folly already: the settings do
    // not need folly::Init.
    if (const char* gids = std::getenv("LSMR_PRELOAD_GIDS")) {
      gflags::SetCommandLineOption("libsoftmetaroce_gid", gids);
    }
    // Diagnostics go to stderr: a preloaded library's log files would vanish.
    gflags::SetCommandLineOption("logtostderr", "1");
    // A C++ host may have initialized folly already, and a second Init
    // throws; skip it when the process has left the Init phase.
    if (folly::get_process_phase() != folly::ProcessPhase::Init) {
      return true;
    }
    // folly::Init keeps references to its arguments; all of this lives
    // forever.
    static int argc = 1;
    static char prog[] = "lsmr_preload";
    static char* argv[] = {prog, nullptr};
    static char** argvp = argv;
    static folly::Init init(&argc, &argvp, /*removeFlags=*/false);
    // folly's singleton-shutdown watchdog SIGABRTs a stalled teardown;
    // keep it as a backstop but give shutdown headroom so a slow finish
    // does not abort an otherwise valid run.
    folly::SingletonVault::singleton()->setShutdownTimeout(
        std::chrono::minutes{30});
    return true;
  }();
  (void)initialized;
}

libsoftmetaroce::MetaRoceVerbs& preloadVerbs() {
  ensureFollyInit();
  // Shared across threads; the backend guards its mutable state internally
  // (folly::Synchronized members, construction-only setup), so no lock here.
  static libsoftmetaroce::MetaRoceVerbs verbs;
  return verbs;
}

int withErrno(int err) {
  errno = err;
  return err;
}

// A verb that reports -1 leaves the reason to errno.
int minusOneSettingErrno(int err) {
  errno = err;
  return -1;
}

// Posting failures name the offending WR through bad_wr. The head is the
// conservative blame when the throw site is unknown, matching the
// backend's own refusals (a null list yields nullptr naturally).
template <typename Wr>
int postErrno(int err, Wr* wr, Wr** bad_wr) {
  if (bad_wr != nullptr) {
    *bad_wr = wr;
  }
  return withErrno(err);
}

// lsmr stores no IOVA mapping: ordinary regions answer by virtual address
// and zero-based regions from their own base. Accept only the base that
// matches the access mode; anything else would address the wrong space.
bool iovaMatchesMapping(void* addr, uint64_t iova, int access) {
  if ((access & IBV_ACCESS_ZERO_BASED) != 0) {
    return iova == 0;
  }
  return iova == reinterpret_cast<uint64_t>(addr);
}

// Datapath verbs are static inline (they dispatch via context->ops), so
// they cannot be preloaded; these trampolines go in the ops table instead.
int lsmrOpsPostSend(ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr** bad) {
  try {
    return preloadVerbs().ibv_post_send(qp, wr, bad);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: post_send failed: %s\n", e.what());
    return postErrno(EIO, wr, bad);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: post_send failed (unknown)\n");
    return postErrno(EIO, wr, bad);
  }
}

int lsmrOpsPostRecv(ibv_qp* qp, ibv_recv_wr* wr, ibv_recv_wr** bad) {
  try {
    return preloadVerbs().ibv_post_recv(qp, wr, bad);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: post_recv failed: %s\n", e.what());
    return postErrno(EIO, wr, bad);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: post_recv failed (unknown)\n");
    return postErrno(EIO, wr, bad);
  }
}

int lsmrOpsPostSrqRecv(ibv_srq* srq, ibv_recv_wr* wr, ibv_recv_wr** bad) {
  try {
    return preloadVerbs().ibv_post_srq_recv(srq, wr, bad);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: post_srq_recv failed: %s\n", e.what());
    return postErrno(EIO, wr, bad);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: post_srq_recv failed (unknown)\n");
    return postErrno(EIO, wr, bad);
  }
}

// poll_cq returns a completion count, not an errno code: a positive value
// would report completions that were never filled, so failures set errno
// and return -1, matching the backend's own convention.
int lsmrOpsPollCq(ibv_cq* cq, int num_entries, ibv_wc* wc) {
  try {
    return preloadVerbs().ibv_poll_cq(cq, num_entries, wc);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: poll_cq failed: %s\n", e.what());
    errno = EIO;
    return -1;
  } catch (...) {
    fprintf(stderr, "lsmr_preload: poll_cq failed (unknown)\n");
    errno = EIO;
    return -1;
  }
}

int lsmrOpsReqNotifyCq(ibv_cq* /*cq*/, int /*solicited_only*/) {
  // Polling-only: arming fails, since no comp channel exists to deliver.
  return withErrno(ENOSYS);
}

} // namespace

extern "C" {

// Control-path verbs: real exported symbols, interposed directly.

ibv_device** FOLLY_NULLABLE ibv_get_device_list(int* num_devices) {
  try {
    // lsmr's ibv_get_device_list dereferences its argument unconditionally,
    // so give it a real pointer and hand the caller's optional out-param
    // back only when it was provided.
    int deviceCount = 0;
    ibv_device** list = preloadVerbs().ibv_get_device_list(&deviceCount);
    if (num_devices) {
      *num_devices = deviceCount;
    }
    return list;
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: get_device_list failed: %s\n", e.what());
    errno = ENODEV;
    return nullptr;
  } catch (...) {
    fprintf(stderr, "lsmr_preload: get_device_list failed (unknown)\n");
    errno = ENODEV;
    return nullptr;
  }
}

void ibv_free_device_list(ibv_device** list) {
  try {
    preloadVerbs().ibv_free_device_list(list);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: free_device_list failed: %s\n", e.what());
    errno = EIO;
  } catch (...) {
    fprintf(stderr, "lsmr_preload: free_device_list failed (unknown)\n");
    errno = EIO;
  }
}

const char* FOLLY_NULLABLE ibv_get_device_name(ibv_device* device) {
  // Borrowed: the API returns a library-owned string the caller must not
  // free, so every call hands back the same pointer without allocating.
  return device ? device->name : nullptr;
}

ibv_context* FOLLY_NULLABLE ibv_open_device(ibv_device* device) {
  try {
    ibv_context* ctx = preloadVerbs().ibv_open_device(device);
    if (ctx) {
      ctx->ops.post_send = lsmrOpsPostSend;
      ctx->ops.post_recv = lsmrOpsPostRecv;
      ctx->ops.post_srq_recv = lsmrOpsPostSrqRecv;
      ctx->ops.poll_cq = lsmrOpsPollCq;
      ctx->ops.req_notify_cq = lsmrOpsReqNotifyCq;
      // Force ibv_query_port down its legacy path (the real symbol we
      // interpose) instead of the uninterposable verbs_context fast path.
      ctx->abi_compat = nullptr;
    }
    return ctx;
  } catch (const std::exception& e) {
    // Construction fails when no usable address exists (an empty namespace,
    // a gid list matching nothing local). Fail the open, not the process.
    fprintf(stderr, "lsmr_preload: open_device failed: %s\n", e.what());
    errno = ENODEV;
    return nullptr;
  } catch (...) {
    fprintf(stderr, "lsmr_preload: open_device failed (unknown)\n");
    errno = ENODEV;
    return nullptr;
  }
}

int ibv_close_device(ibv_context* context) {
  try {
    return preloadVerbs().ibv_close_device(context);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: close_device failed: %s\n", e.what());
    return minusOneSettingErrno(EIO);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: close_device failed (unknown)\n");
    return minusOneSettingErrno(EIO);
  }
}

// The extended query is a static inline that falls back to this real
// symbol when no provider ops exist; extended capabilities then read as
// unsupported, which callers already handle as feature absence.
int ibv_query_device(ibv_context* context, ibv_device_attr* device_attr) {
  try {
    return preloadVerbs().ibv_query_device(context, device_attr);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: query_device failed: %s\n", e.what());
    return withErrno(EIO);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: query_device failed (unknown)\n");
    return withErrno(EIO);
  }
}

// Size of rdma-core's private legacy port attributes, a strict prefix of
// the modern struct (fields through `flags`); the compat symbol promises
// to write only this much. Modern callers reach us through the header's
// inline, which zeroes their full-size buffer first, so copying the prefix
// reproduces real libibverbs behavior for both caller shapes.
constexpr size_t kLegacyPortAttrSize = 48;

int ibv_query_port(
    ibv_context* context,
    uint8_t port_num,
    _compat_ibv_port_attr* port_attr) {
  static_assert(
      offsetof(ibv_port_attr, flags) + sizeof(uint8_t) == kLegacyPortAttrSize);
  static_assert(sizeof(ibv_port_attr) >= kLegacyPortAttrSize);
  try {
    if (port_attr == nullptr) {
      return withErrno(EINVAL);
    }
    // The backend clears the modern size, so query a temporary and copy only
    // the legacy prefix; a true legacy-size caller buffer must not see the
    // tail. The member was declared while the query-port macro was active, so
    // it carries the expanded spelling; if a future verbs.h drops the
    // redirect, this call fails to compile with the fix beside it.
    ibv_port_attr queried{};
    int ret = preloadVerbs().___ibv_query_port(context, port_num, &queried);
    if (ret != 0) {
      return ret;
    }
    // Both sides are fixed 48-byte buffers, pinned by the static_asserts
    // above; same size by construction, so no overflow path.
    // NOLINTNEXTLINE(facebook-security-vulnerable-memcpy)
    memcpy(port_attr, &queried, kLegacyPortAttrSize);
    return 0;
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: query_port failed: %s\n", e.what());
    return withErrno(EIO);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: query_port failed (unknown)\n");
    return withErrno(EIO);
  }
}

int ibv_query_gid(
    ibv_context* context,
    uint8_t port_num,
    int index,
    ibv_gid* gid) {
  try {
    return preloadVerbs().ibv_query_gid(context, port_num, index, gid);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: query_gid failed: %s\n", e.what());
    return minusOneSettingErrno(EIO);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: query_gid failed (unknown)\n");
    return minusOneSettingErrno(EIO);
  }
}

int _ibv_query_gid_ex(
    ibv_context* context,
    uint32_t port_num,
    uint32_t gid_index,
    ibv_gid_entry* entry,
    uint32_t flags,
    size_t entry_size) {
  try {
    if (entry == nullptr) {
      return withErrno(EINVAL);
    }
    if (flags != 0) {
      return withErrno(EOPNOTSUPP);
    }
    if (entry_size < sizeof(ibv_gid_entry) || gid_index > INT_MAX) {
      return withErrno(EINVAL);
    }
    return preloadVerbs().ibv_query_gid_ex(
        context,
        static_cast<uint8_t>(port_num),
        static_cast<int>(gid_index),
        entry,
        flags);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: query_gid_ex failed: %s\n", e.what());
    return withErrno(EIO);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: query_gid_ex failed (unknown)\n");
    return withErrno(EIO);
  }
}

// lsmr tracks no per-gid type; everything it serves is RoCE v2.
int ibv_query_gid_type(
    ibv_context* context,
    uint8_t port_num,
    unsigned int index,
    ibv_gid_type* type) {
  try {
    if (type == nullptr) {
      return minusOneSettingErrno(EINVAL);
    }
    if (index > INT_MAX) {
      return minusOneSettingErrno(EINVAL);
    }
    ibv_gid gid{};
    int ret = preloadVerbs().ibv_query_gid(
        context, port_num, static_cast<int>(index), &gid);
    // The backend shares the -1 convention today, but enforce the shape
    // here: this symbol must never return a positive errno on failure,
    // nor -1 with errno unset.
    if (ret != 0) {
      return minusOneSettingErrno(errno ? errno : EIO);
    }
    *type = IBV_GID_TYPE_ROCE_V2;
    return 0;
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: query_gid_type failed: %s\n", e.what());
    return minusOneSettingErrno(EIO);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: query_gid_type failed (unknown)\n");
    return minusOneSettingErrno(EIO);
  }
}

ibv_pd* FOLLY_NULLABLE ibv_alloc_pd(ibv_context* context) {
  try {
    ibv_pd* pd = preloadVerbs().ibv_alloc_pd(context);
    if (pd) {
      // Anchor pd->context to the ops-populated context so QPs/CQs created
      // from it inherit the datapath ops table.
      pd->context = context;
    }
    return pd;
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: alloc_pd failed: %s\n", e.what());
    errno = EIO;
    return nullptr;
  } catch (...) {
    fprintf(stderr, "lsmr_preload: alloc_pd failed (unknown)\n");
    errno = EIO;
    return nullptr;
  }
}

int ibv_dealloc_pd(ibv_pd* pd) {
  try {
    return preloadVerbs().ibv_dealloc_pd(pd);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: dealloc_pd failed: %s\n", e.what());
    return withErrno(EIO);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: dealloc_pd failed (unknown)\n");
    return withErrno(EIO);
  }
}

ibv_cq* FOLLY_NULLABLE ibv_create_cq(
    ibv_context* context,
    int cqe,
    void* cq_context,
    ibv_comp_channel* channel,
    int comp_vector) {
  try {
    return preloadVerbs().ibv_create_cq(
        context, cqe, cq_context, channel, comp_vector);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: create_cq failed: %s\n", e.what());
    errno = EIO;
    return nullptr;
  } catch (...) {
    fprintf(stderr, "lsmr_preload: create_cq failed (unknown)\n");
    errno = EIO;
    return nullptr;
  }
}

int ibv_destroy_cq(ibv_cq* cq) {
  try {
    return preloadVerbs().ibv_destroy_cq(cq);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: destroy_cq failed: %s\n", e.what());
    return withErrno(EIO);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: destroy_cq failed (unknown)\n");
    return withErrno(EIO);
  }
}

// ibv_reg_mr is a macro expanding to one of these underlying symbols
// depending on the access flags; interpose all three.
ibv_mr* FOLLY_NULLABLE
ibv_reg_mr(ibv_pd* pd, void* addr, size_t length, int access) {
  try {
    return preloadVerbs().ibv_reg_mr_internal(pd, addr, length, access);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: reg_mr failed: %s\n", e.what());
    errno = EIO;
    return nullptr;
  } catch (...) {
    fprintf(stderr, "lsmr_preload: reg_mr failed (unknown)\n");
    errno = EIO;
    return nullptr;
  }
}

ibv_mr* FOLLY_NULLABLE ibv_reg_mr_iova(
    ibv_pd* pd,
    void* addr,
    size_t length,
    uint64_t iova,
    int access) {
  if (!iovaMatchesMapping(addr, iova, access)) {
    errno = EINVAL;
    return nullptr;
  }
  try {
    return preloadVerbs().ibv_reg_mr_internal(pd, addr, length, access);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: reg_mr_iova failed: %s\n", e.what());
    errno = EIO;
    return nullptr;
  } catch (...) {
    fprintf(stderr, "lsmr_preload: reg_mr_iova failed (unknown)\n");
    errno = EIO;
    return nullptr;
  }
}

ibv_mr* FOLLY_NULLABLE ibv_reg_mr_iova2(
    ibv_pd* pd,
    void* addr,
    size_t length,
    uint64_t iova,
    unsigned int access) {
  if (!iovaMatchesMapping(addr, iova, static_cast<int>(access))) {
    errno = EINVAL;
    return nullptr;
  }
  try {
    return preloadVerbs().ibv_reg_mr_internal(
        pd, addr, length, static_cast<int>(access));
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: reg_mr_iova2 failed: %s\n", e.what());
    errno = EIO;
    return nullptr;
  } catch (...) {
    fprintf(stderr, "lsmr_preload: reg_mr_iova2 failed (unknown)\n");
    errno = EIO;
    return nullptr;
  }
}

int ibv_dereg_mr(ibv_mr* mr) {
  try {
    return preloadVerbs().ibv_dereg_mr(mr);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: dereg_mr failed: %s\n", e.what());
    return withErrno(EIO);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: dereg_mr failed (unknown)\n");
    return withErrno(EIO);
  }
}

ibv_qp* FOLLY_NULLABLE
ibv_create_qp(ibv_pd* pd, ibv_qp_init_attr* qp_init_attr) {
  try {
    ibv_qp* qp = preloadVerbs().ibv_create_qp(pd, qp_init_attr);
    if (qp && pd) {
      // The core leaves qp->context null; the inlined post_send/post_recv
      // dispatch via qp->context->ops, so point it at the pd's context.
      qp->context = pd->context;
    }
    return qp;
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: create_qp failed: %s\n", e.what());
    errno = EIO;
    return nullptr;
  } catch (...) {
    fprintf(stderr, "lsmr_preload: create_qp failed (unknown)\n");
    errno = EIO;
    return nullptr;
  }
}

int ibv_modify_qp(ibv_qp* qp, ibv_qp_attr* attr, int attr_mask) {
  try {
    return preloadVerbs().ibv_modify_qp(qp, attr, attr_mask);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: modify_qp failed: %s\n", e.what());
    return withErrno(EIO);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: modify_qp failed (unknown)\n");
    return withErrno(EIO);
  }
}

int ibv_query_qp(
    ibv_qp* qp,
    ibv_qp_attr* attr,
    int attr_mask,
    ibv_qp_init_attr* init_attr) {
  try {
    return preloadVerbs().ibv_query_qp(qp, attr, attr_mask, init_attr);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: query_qp failed: %s\n", e.what());
    return withErrno(EIO);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: query_qp failed (unknown)\n");
    return withErrno(EIO);
  }
}

int ibv_destroy_qp(ibv_qp* qp) {
  try {
    return preloadVerbs().ibv_destroy_qp(qp);
  } catch (const std::exception& e) {
    fprintf(stderr, "lsmr_preload: destroy_qp failed: %s\n", e.what());
    return withErrno(EIO);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: destroy_qp failed (unknown)\n");
    return withErrno(EIO);
  }
}

ibv_comp_channel* FOLLY_NULLABLE
ibv_create_comp_channel(ibv_context* /*context*/) {
  // Polling-only: refuse even if the backend grows channels, so a binary
  // always learns at creation that events are unavailable.
  errno = ENOSYS;
  return nullptr;
}

int ibv_destroy_comp_channel(ibv_comp_channel* channel) {
  try {
    return preloadVerbs().ibv_destroy_comp_channel(channel);
  } catch (const std::exception& e) {
    fprintf(
        stderr, "lsmr_preload: destroy_comp_channel failed: %s\n", e.what());
    return withErrno(EIO);
  } catch (...) {
    fprintf(stderr, "lsmr_preload: destroy_comp_channel failed (unknown)\n");
    return withErrno(EIO);
  }
}

// Null-MR and device-memory verbs are static inlines that fail cleanly
// once provider ops are absent, so they need no interposer.

} // extern "C"
