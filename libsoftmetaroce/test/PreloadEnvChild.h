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

// Child-mode entry point for the interposer ABI test binary, used by main
// to run the child check instead of the test suite. The
// EnvGidsHonoredInFreshProcess parent re-executes the binary with
// kPreloadEnvChildArg in a fresh process with LSMR_PRELOAD_GIDS set; main
// runs this instead of the suite so the check exists nowhere as a TEST
// that standalone runs would skip. Reads the requested address from
// LSMR_PRELOAD_GIDS, prints diagnostics to stderr, and returns 0 on
// success.

constexpr char kPreloadEnvChildArg[] = "--lsmr-preload-env-child";

int RunPreloadEnvChild();
