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

#include <folly/Conv.h>
#include <folly/String.h>
#include <libsoftmetaroce/congestion_control/Baseline.h>
#include <libsoftmetaroce/congestion_control/BaselineWithRCN.h>
#include <libsoftmetaroce/congestion_control/CongestionController.h>
#include <libsoftmetaroce/congestion_control/FastStart.h>
#include <libsoftmetaroce/congestion_control/StaticQwnd.h>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace libsoftmetaroce {

/**
 * Interface to create CongestionController instance.
 */
class CongestionControllerFactory {
 public:
  ~CongestionControllerFactory() = default;

  // Build a controller from a --libsoftmetaroce_cc_algorithm spec: an algorithm
  // name with optional comma-separated constructor args after a colon, e.g.
  // "baseline_rcn", "baseline:2,1", "static:128". Args are positional; an
  // omitted trailing arg uses that constructor's default. Returns nullptr for
  // an unknown name, an unparseable or out-of-range arg, or more args than the
  // algorithm takes -- the gflag validator turns that into a startup ("invalid
  // argument") error.
  std::unique_ptr<CongestionController> makeCongestionController(
      const std::string& spec) {
    const folly::StringPiece specPiece{spec};
    folly::StringPiece name = specPiece;
    std::vector<folly::StringPiece> args;
    if (auto colon = spec.find(':'); colon != std::string::npos) {
      name = specPiece.subpiece(0, colon);
      // "name:" with no args means "use defaults"; folly::split on "" yields
      // one empty field that fails arg parsing, so only split a non-empty arg
      // list.
      if (const auto argsPiece = specPiece.subpiece(colon + 1);
          !argsPiece.empty()) {
        folly::split(',', argsPiece, args);
      }
    }
    bool ok = true;
    if (name == "baseline_rcn") {
      auto epsilon = arg<int>(args, 0, 2, ok);
      auto beta = shiftArg(args, 1, Baseline::kDefaultBeta, ok);
      auto gammaNumerator =
          arg<uint32_t>(args, 2, BaselineWithRCN::kDefaultGammaNumerator, ok);
      auto omegaNs =
          arg<uint32_t>(args, 3, BaselineWithRCN::kDefaultOmegaNs, ok);
      auto lambda = shiftArg(args, 4, Baseline::kDefaultLambda, ok);
      // gammaNumerator divides the qwnd_max computation, so a zero from the
      // spec string is an invalid argument, reported as one rather than
      // reaching the constructor.
      if (gammaNumerator == 0) {
        ok = false;
      }
      return (ok && args.size() <= 5)
          ? std::make_unique<BaselineWithRCN>(
                epsilon, beta, gammaNumerator, omegaNs, lambda)
          : nullptr;
    }
    if (name == "baseline") {
      auto epsilon = arg<int>(args, 0, 2, ok);
      auto beta = shiftArg(args, 1, Baseline::kDefaultBeta, ok);
      auto lambda = shiftArg(args, 2, Baseline::kDefaultLambda, ok);
      return (ok && args.size() <= 3)
          ? std::make_unique<Baseline>(epsilon, beta, lambda)
          : nullptr;
    }
    if (name == "faststart") {
      return args.empty() ? std::make_unique<FastStart>() : nullptr;
    }
    if (name == "static") {
      auto fwnd = arg<uint16_t>(args, 0, 5, ok);
      return (ok && args.size() <= 1) ? std::make_unique<StaticQwnd>(fwnd)
                                      : nullptr;
    }
    return nullptr; // unknown algorithm
  }

 private:
  // The i-th positional arg parsed as T, or deflt when absent; clears ok on a
  // present-but-unparseable arg.
  template <class T>
  static T arg(
      const std::vector<folly::StringPiece>& args,
      size_t i,
      T deflt,
      bool& ok) {
    if (i >= args.size()) {
      return deflt;
    }
    auto parsed = folly::tryTo<T>(args[i]);
    if (parsed.hasValue()) {
      return parsed.value();
    }
    ok = false;
    return deflt;
  }

  // As arg<int>, also clearing ok on a value that cannot shift an int.
  static int shiftArg(
      const std::vector<folly::StringPiece>& args,
      size_t i,
      int deflt,
      bool& ok) {
    const int shift = arg<int>(args, i, deflt, ok);
    if (shift < 0 || shift >= std::numeric_limits<int>::digits) {
      ok = false;
    }
    return shift;
  }
};
} // namespace libsoftmetaroce
