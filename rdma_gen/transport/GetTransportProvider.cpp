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

#include "rdma_gen/transport/GetTransportProvider.h"

#include <glog/logging.h>

#include "libsoftmetaroce/MetaRoceVerbs.h"
#include "rdma_gen/Common.h"
#include "rdma_gen/transport/VerbsProvider.h"

namespace facebook::neteng::ai::rdma_gen {

std::unique_ptr<ITransportProvider> getTransportProvider(
    const Params& params,
    std::shared_ptr<VerbsPeerPolicy> peer_policy) {
  switch (params.verbs_mode) {
    case VerbsMode::LibSoftMetaRoCE:
      LOG(INFO) << "Using libsoftmetaroce verbs";
      return std::make_unique<VerbsProvider>(
          std::make_shared<libsoftmetaroce::MetaRoceVerbs>(),
          std::move(peer_policy));
    default:
      LOG(INFO) << "Using default rdma verbs";
      return std::make_unique<VerbsProvider>(
          std::make_shared<facebook::neteng::ai::RdmaVerbs>(),
          std::move(peer_policy));
  }
}

} // namespace facebook::neteng::ai::rdma_gen
