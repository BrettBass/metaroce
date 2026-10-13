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

#pragma once

#include <atomic>
#include <cstddef>
#include <string_view>
#include <type_traits>

#include <infiniband/verbs.h>

namespace libsoftmetaroce {

// `ibv_qp_state` is an unscoped C enum, so a stream renders it as its number.
constexpr std::string_view qpStateToString(ibv_qp_state state) noexcept {
  switch (state) {
    case IBV_QPS_RESET:
      return "RESET";
    case IBV_QPS_INIT:
      return "INIT";
    case IBV_QPS_RTR:
      return "RTR";
    case IBV_QPS_RTS:
      return "RTS";
    case IBV_QPS_SQD:
      return "SQD";
    case IBV_QPS_SQE:
      return "SQE";
    case IBV_QPS_ERR:
      return "ERR";
    case IBV_QPS_UNKNOWN:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

// The window a queue pair is configured in: `rq_psn` and `path_mtu` are set
// on the INIT to RTR transition, so they are read while it is still open.
constexpr bool isBeforeRtr(ibv_qp_state state) noexcept {
  return state == IBV_QPS_RESET || state == IBV_QPS_INIT;
}

// The wider window, open until the queue pair runs. The attributes that govern
// retransmission are frozen at its close, so a timer never sees one change.
constexpr bool isBeforeRts(ibv_qp_state state) noexcept {
  return isBeforeRtr(state) || state == IBV_QPS_RTR;
}

// How ibv_modify_qp treats a requested state transition.
enum class QpTransition {
  SUPPORTED,
  LEGAL_BUT_UNSUPPORTED,
  INVALID,
};

// Classifies a requested QP state transition: performed, refused as
// unimplemented, or rejected as outside the state table. UNKNOWN is not a
// state, and SQE is never a valid modify target.
constexpr QpTransition classifyQpTransition(
    ibv_qp_state from,
    ibv_qp_state to) noexcept {
  switch (from) {
    case IBV_QPS_RESET:
    case IBV_QPS_INIT:
    case IBV_QPS_RTR:
    case IBV_QPS_RTS:
    case IBV_QPS_SQD:
    case IBV_QPS_SQE:
    case IBV_QPS_ERR:
      break;
    case IBV_QPS_UNKNOWN:
      return QpTransition::INVALID;
    default:
      return QpTransition::INVALID;
  }
  // Self-transitions perform, except from SQE.
  if (from == to && from != IBV_QPS_SQE) {
    return QpTransition::SUPPORTED;
  }
  switch (to) {
    case IBV_QPS_RESET:
      // Every state recycles through RESET.
      return QpTransition::LEGAL_BUT_UNSUPPORTED;
    case IBV_QPS_ERR:
      // Every state except RESET moves to ERR.
      return from == IBV_QPS_RESET ? QpTransition::INVALID
                                   : QpTransition::SUPPORTED;
    case IBV_QPS_INIT:
      return from == IBV_QPS_RESET ? QpTransition::SUPPORTED
                                   : QpTransition::INVALID;
    case IBV_QPS_RTR:
      return from == IBV_QPS_INIT ? QpTransition::SUPPORTED
                                  : QpTransition::INVALID;
    case IBV_QPS_RTS:
      // Forward bring-up performs; resuming from SQD or SQE is unimplemented.
      if (from == IBV_QPS_RTR) {
        return QpTransition::SUPPORTED;
      }
      return (from == IBV_QPS_SQD || from == IBV_QPS_SQE)
          ? QpTransition::LEGAL_BUT_UNSUPPORTED
          : QpTransition::INVALID;
    case IBV_QPS_SQD:
      // Suspending a running QP is unimplemented.
      return from == IBV_QPS_RTS ? QpTransition::LEGAL_BUT_UNSUPPORTED
                                 : QpTransition::INVALID;
    case IBV_QPS_SQE:
    case IBV_QPS_UNKNOWN:
      return QpTransition::INVALID;
    default:
      return QpTransition::INVALID;
  }
}

// The concurrent accessors for ibv_qp::state. Datapath error checks read the
// field without a lock while several guards write it (the TagSet mutex, a
// Tag's send map, a Tag's receive map), so every access once workers may run
// goes through these. Initialization before publication and inspection after
// workers join stay plain. The field keeps its C layout: the asserts hold
// the alignment an atomic_ref needs and the lock-freedom the datapath reads
// want. All three are sequentially consistent; no caller has shown a weaker
// ordering correct.
static_assert(std::is_trivially_copyable_v<ibv_qp_state>);
static_assert(
    alignof(struct ibv_qp) %
            std::atomic_ref<ibv_qp_state>::required_alignment ==
        0,
    "any aligned ibv_qp must align state for atomic_ref");
static_assert(
    offsetof(struct ibv_qp, state) %
            std::atomic_ref<ibv_qp_state>::required_alignment ==
        0,
    "state must sit at an atomic_ref-aligned offset");
static_assert(
    std::atomic_ref<ibv_qp_state>::is_always_lock_free,
    "datapath state reads must not take a lock");

[[nodiscard]] inline ibv_qp_state loadQpState(struct ibv_qp& qp) noexcept {
  return std::atomic_ref<ibv_qp_state>(qp.state).load(
      std::memory_order_seq_cst);
}

// Stores ERR. Concurrent stores write the same value, so any interleaving
// leaves ERR.
inline void storeQpErrorState(struct ibv_qp& qp) noexcept {
  std::atomic_ref<ibv_qp_state>(qp.state).store(
      IBV_QPS_ERR, std::memory_order_seq_cst);
}

// Stores `desired` where the state still holds `expected`, reporting whether
// it did. On failure `expected` holds what beat it, so a caller looping back
// sees the concurrent transition rather than its own stale read.
[[nodiscard]] inline bool compareAndSetQpState(
    struct ibv_qp& qp,
    ibv_qp_state& expected,
    ibv_qp_state desired) noexcept {
  return std::atomic_ref<ibv_qp_state>(qp.state).compare_exchange_strong(
      expected, desired, std::memory_order_seq_cst);
}

} // namespace libsoftmetaroce
