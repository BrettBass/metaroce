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

#include "rdma_gen/Common.h"
#include "rdma_gen/RdmaGenUtils.h"

namespace facebook::neteng::ai::rdma_gen {

std::string getCommandLineStr(int argc, char* argv[]);

// Builds rdma_gen Params from the process gflags. The caller must have already
// run folly::Init so the flags are parsed. Defined in RdmaGenMain.cpp, where
// the rdma_mode gflag is defined.
Params buildParamsFromFlags();

int clientMain(
    Params& params,
    OperationType opType,
    RdmaGenResources& resources);

int serverMain(
    Params& params,
    OperationType opType,
    RdmaGenResources& resources);

// Dispatches to clientMain or serverMain based on FLAGS_rc_servers (and sets
// the server rx_depth from FLAGS_buffer_cap). Shared so RdmaGenMain and
// alternate mains keep identical client/server dispatch behavior; callers
// inject the transport via resources.
int runClientOrServer(
    Params& params,
    OperationType opType,
    RdmaGenResources&& resources);

} // namespace facebook::neteng::ai::rdma_gen
