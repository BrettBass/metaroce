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

#include <glog/logging.h>
#include <cstdint>
#include <string_view>
#include <vector>

#include <libsoftmetaroce/RejectedPosnSet.h>

namespace libsoftmetaroce {

// A posn's state in full, held beside the counts a work request keeps so that
// they can be checked against what actually happened. Message-proportional by
// design, and off unless asked for.
class PosnAuditor {
 public:
  enum class State : uint8_t { PENDING, ALLOCATED, REJECTED, RESOLVED };

  static constexpr std::string_view name(State state) noexcept {
    switch (state) {
      case State::PENDING:
        return "pending";
      case State::ALLOCATED:
        return "allocated";
      case State::REJECTED:
        return "rejected";
      case State::RESOLVED:
        return "resolved";
    }
    return "unknown"; // unreachable; satisfies -Wreturn-type
  }

  // The two counts are the work request's own, read back after every event to
  // see whether they still say what the posns do.
  PosnAuditor(
      uint32_t numPosn,
      const uint32_t& outstandingPosnCount,
      const RejectedPosnSet& rejectedPosns) noexcept
      : outstandingPosnCount_(outstandingPosnCount),
        rejectedPosns_(rejectedPosns),
        state_(numPosn, State::PENDING),
        outstanding_{numPosn} {}

  void onAllocated(uint32_t posn) noexcept {
    CHECK(state_[posn] == State::PENDING || state_[posn] == State::REJECTED)
        << "posn " << posn << " went on the wire while " << name(state_[posn]);
    rejected_ -= state_[posn] == State::REJECTED ? 1 : 0;
    state_[posn] = State::ALLOCATED;
    checkCounts();
  }

  void onReleased(uint32_t posn) noexcept {
    CHECK(state_[posn] == State::ALLOCATED)
        << "posn " << posn << " was handed back while " << name(state_[posn])
        << ", so the frame it names is not the one on the wire";
    state_[posn] = State::REJECTED;
    ++rejected_;
    checkCounts();
  }

  void onResolved(uint32_t posn) noexcept {
    CHECK(state_[posn] != State::RESOLVED)
        << "posn " << posn << " resolved a second time, which takes it off "
        << "the outstanding count twice";
    rejected_ -= state_[posn] == State::REJECTED ? 1 : 0;
    state_[posn] = State::RESOLVED;
    --outstanding_;
    checkCounts();
  }

  // The one event whose posn comes off the wire; the receive path naks one
  // failing isPosnOutOfRange(), so posn indexes within state_.
  void onReceived(uint32_t posn) noexcept {
    if (state_[posn] != State::PENDING) {
      LOG_FIRST_N(ERROR, 10)
          << "posn audit: posn " << posn << " of " << state_.size()
          << " arrived while " << name(state_[posn])
          << ", so the sender put one posn on two fsns and the message "
             "is short the posn this one stands in for";
    }
    if (state_[posn] == State::RESOLVED) {
      ++posnsTheSenderReused_;
    } else {
      state_[posn] = State::RESOLVED;
      --outstanding_;
    }
    checkCounts();
  }

  // A receive shortened to its terminal extent keeps no posn beyond it.
  void truncateTo(uint32_t numPosn) noexcept {
    CHECK_LE(numPosn, state_.size()) << "a receive shortened to " << numPosn
                                     << " posns cannot be longer than the "
                                     << state_.size() << " it was posted for";
    for (uint32_t posn = numPosn; posn < state_.size(); ++posn) {
      CHECK(state_[posn] == State::PENDING)
          << "posn " << posn << " is " << name(state_[posn])
          << " beyond the last packet the sender said it would send";
    }
    outstanding_ -= static_cast<uint32_t>(state_.size()) - numPosn;
    state_.resize(numPosn);
    checkCounts();
  }

  void markFailing() noexcept {
    for (auto& state : state_) {
      if (state == State::PENDING || state == State::REJECTED) {
        state = State::RESOLVED;
        --outstanding_;
      }
    }
    rejected_ = 0;
    // Failing takes both counts to zero, so the skew goes with them.
    posnsTheSenderReused_ = 0;
    checkCounts();
  }

  // The count and the sum cannot answer this: arrivals aliasing a shorter
  // message sum the same as arrivals within it.
  [[nodiscard]] bool anyArrivedAtOrAbove(uint32_t posn) const noexcept {
    for (uint32_t i = posn; i < state_.size(); ++i) {
      if (state_[i] != State::PENDING) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] State stateOf(uint32_t posn) const noexcept {
    CHECK_LT(posn, state_.size()) << "posn " << posn << " is past the "
                                  << state_.size() << " this message has";
    return state_[posn];
  }

 private:
  // The counts a work request holds, against the posns actually in those
  // states. Disagreeing means one of the two has lost track.
  void checkCounts() const noexcept {
    // A reused posn takes the work request's count down twice, so the two
    // part by exactly those arrivals until the message fails.
    CHECK_EQ(outstanding_, outstandingPosnCount_ + posnsTheSenderReused_)
        << "the outstanding count and the posns that have not resolved "
           "disagree";
    CHECK_EQ(rejected_, rejectedPosns_.count())
        << "the rejected count and the posns waiting to go out again disagree";
  }

  // Injected dependencies: the work request's own counts, read back after
  // each event.
  const uint32_t& outstandingPosnCount_;
  const RejectedPosnSet& rejectedPosns_;

  // Protocol / runtime state.
  std::vector<State> state_;
  uint32_t outstanding_{0};
  uint32_t rejected_{0};
  uint32_t posnsTheSenderReused_{0};
};

} // namespace libsoftmetaroce
