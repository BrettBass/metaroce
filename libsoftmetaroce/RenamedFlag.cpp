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

#include <libsoftmetaroce/RenamedFlag.h>

#include <mutex>
#include <set>
#include <string>

namespace libsoftmetaroce {

bool claimRenamedFlagWarning(const char* formerName) {
  static std::mutex mutex;
  static std::set<std::string> warned;
  std::lock_guard<std::mutex> guard(mutex);
  return warned.insert(formerName).second;
}

} // namespace libsoftmetaroce
