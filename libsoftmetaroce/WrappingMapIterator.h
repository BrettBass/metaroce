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

#include <iterator>
#include <map>
#include <memory>
#include <type_traits>

namespace libsoftmetaroce {

template <typename KeyType, typename ValueType>
class WrappingMapIterator {
 public:
  // Iterator type definitions required for standard algorithms
  using value_type =
      std::pair<const KeyType&, const std::unique_ptr<ValueType>&>;
  using reference = value_type;
  using pointer = void;
  using difference_type = std::ptrdiff_t;
  using iterator_category = std::forward_iterator_tag;

  WrappingMapIterator(
      const std::map<KeyType, std::unique_ptr<ValueType>>& map,
      KeyType startKey)
      : map_(&map), wrapped_(false), finished_(false) {
    if (map.empty()) {
      finished_ = true;
      return;
    }

    // Find the first key > startKey
    current_ = map.upper_bound(startKey);

    // If no key > startKey exists, wrap to beginning
    if (current_ == map.end()) {
      current_ = map.begin();
      wrapped_ = true;
    }

    start_ = current_;
  }

  // End iterator constructor
  WrappingMapIterator() : map_(nullptr), wrapped_(false), finished_(true) {}

  std::pair<const KeyType&, const std::unique_ptr<ValueType>&> operator*()
      const {
    return {current_->first, current_->second};
  }

  WrappingMapIterator& operator++() {
    if (finished_) {
      return *this;
    }

    ++current_;

    if (current_ == map_->end()) {
      if (wrapped_) {
        finished_ = true;
      } else {
        current_ = map_->begin();
        wrapped_ = true;
      }
    }

    // Check if we've completed a full cycle
    if (!finished_ && wrapped_ && current_ == start_) {
      finished_ = true;
    }

    return *this;
  }

  WrappingMapIterator operator++(int) {
    WrappingMapIterator tmp = *this;
    ++(*this);
    return tmp;
  }

  bool operator==(const WrappingMapIterator& other) const {
    if (finished_ && other.finished_) {
      return true;
    }
    if (finished_ != other.finished_) {
      return false;
    }
    return map_ == other.map_ && current_ == other.current_ &&
        wrapped_ == other.wrapped_;
  }

  bool operator!=(const WrappingMapIterator& other) const {
    return !(*this == other);
  }

 private:
  const std::map<KeyType, std::unique_ptr<ValueType>>* map_;
  typename std::map<KeyType, std::unique_ptr<ValueType>>::const_iterator
      current_;
  typename std::map<KeyType, std::unique_ptr<ValueType>>::const_iterator start_;
  bool wrapped_;
  bool finished_;
};

template <typename KeyType, typename ValueType>
class WrappingMapRange {
 public:
  WrappingMapRange(
      const std::map<KeyType, std::unique_ptr<ValueType>>& map,
      KeyType startKey)
      : map_(map), startKey_(startKey) {}

  WrappingMapIterator<KeyType, ValueType> begin() const {
    return WrappingMapIterator<KeyType, ValueType>(map_, startKey_);
  }

  WrappingMapIterator<KeyType, ValueType> end() const {
    return WrappingMapIterator<KeyType, ValueType>(); // End iterator
  }

 private:
  const std::map<KeyType, std::unique_ptr<ValueType>>& map_;
  KeyType startKey_;
};

/**
 * Creates an iterator that starts from the first key > n and wraps around.
 *
 * @param map The ordered map to iterate over
 * @param n The starting value (iteration begins from first key > n)
 * @return A range object that can be used in range-based for loops
 *
 * Example usage:
 *   std::map<uint8_t, std::unique_ptr<MyClass>> myMap;
 *   // ... populate map with keys [0, 1, 253, 254, 255]
 *
 *   for (const auto [k, v] : iterateFrom(myMap, 254)) {
 *       // Will iterate in order: 255, 0, 1, 253, 254
 *       std::cout << "Key: " << static_cast<int>(k) << std::endl;
 *   }
 */
template <typename KeyType, typename ValueType>
auto iterateFrom(
    const std::map<KeyType, std::unique_ptr<ValueType>>& map,
    KeyType n) {
  static_assert(
      std::is_unsigned_v<KeyType>, "KeyType must be an unsigned integer type");
  return WrappingMapRange<KeyType, ValueType>(map, n);
}

} // namespace libsoftmetaroce
