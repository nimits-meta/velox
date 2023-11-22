/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#include <folly/container/F14Set.h>

#include "velox/common/base/IOUtils.h"
#include "velox/common/memory/HashStringAllocator.h"
#include "velox/exec/AddressableNonNullValueList.h"
#include "velox/exec/Strings.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::aggregate::prestosql {

namespace detail {

/// Maintains a set of unique values. Non-null values are stored in F14FastSet.
/// A separate flag tracks presence of the null value.
template <
    typename T,
    typename Hash = std::hash<T>,
    typename EqualTo = std::equal_to<T>>
struct SetAccumulator {
  std::optional<vector_size_t> nullIndex;

  folly::F14FastMap<
      T,
      int32_t,
      Hash,
      EqualTo,
      AlignedStlAllocator<std::pair<const T, vector_size_t>, 16>>
      uniqueValues;

  SetAccumulator(const TypePtr& /*type*/, HashStringAllocator* allocator)
      : uniqueValues{AlignedStlAllocator<std::pair<const T, vector_size_t>, 16>(
            allocator)} {}

  SetAccumulator(Hash hash, EqualTo equalTo, HashStringAllocator* allocator)
      : uniqueValues{
            0,
            hash,
            equalTo,
            AlignedStlAllocator<std::pair<const T, vector_size_t>, 16>(
                allocator)} {}

  /// Adds value if new. No-op if the value was added before.
  void addValue(
      const DecodedVector& decoded,
      vector_size_t index,
      HashStringAllocator* /*allocator*/) {
    const auto cnt = uniqueValues.size();
    if (decoded.isNullAt(index)) {
      if (!nullIndex.has_value()) {
        nullIndex = cnt;
      }
    } else {
      uniqueValues.insert(
          {decoded.valueAt<T>(index), nullIndex.has_value() ? cnt + 1 : cnt});
    }
  }

  /// Adds new values from an array.
  void addValues(
      const ArrayVector& arrayVector,
      vector_size_t index,
      const DecodedVector& values,
      HashStringAllocator* allocator) {
    const auto size = arrayVector.sizeAt(index);
    const auto offset = arrayVector.offsetAt(index);

    for (auto i = 0; i < size; ++i) {
      addValue(values, offset + i, allocator);
    }
  }

  void readNullIndex(common::InputByteStream& stream) {
    VELOX_CHECK(!nullIndex.has_value());
    auto serializedNullIndex = stream.read<vector_size_t>();
    if (serializedNullIndex != kNoNullIndex) {
      nullIndex = serializedNullIndex;
    }
  }

  void deserialize(
      const StringView& serialized,
      HashStringAllocator* /*allocator*/) {
    // The serialized value is the nullOffset (kNoNullIndex if no null is
    // present) followed by the unique values ordered by index.
    common::InputByteStream stream(serialized.data());
    readNullIndex(stream);

    size_t index = 0;
    auto size = serialized.size();
    while (stream.offset() < size) {
      if (!nullIndex.has_value() || index != nullIndex.value()) {
        // This is a valid uniqueValue.
        uniqueValues.insert({stream.read<T>(), index});
      }
      index++;
    }
  }

  /// Returns number of unique values including null.
  size_t size() const {
    return uniqueValues.size() + (nullIndex.has_value() ? 1 : 0);
  }

  /// Copies the unique values and null into the specified vector starting at
  /// the specified offset.
  vector_size_t extractValues(FlatVector<T>& values, vector_size_t offset) {
    for (auto value : uniqueValues) {
      values.set(offset + value.second, value.first);
    }

    if (nullIndex.has_value()) {
      values.setNull(offset + nullIndex.value(), true);
    }

    return nullIndex.has_value() ? uniqueValues.size() + 1
                                 : uniqueValues.size();
  }

  void serializeNullIndex(char* buffer) {
    auto serializedNullIndex =
        nullIndex.has_value() ? nullIndex.value() : kNoNullIndex;
    memcpy(buffer, &serializedNullIndex, kVectorSizeT);
  }

  /// Extracts in result[index] a serialized VARBINARY for the Set Values.
  /// This is used for the spill of this accumulator.
  void serialize(const VectorPtr& result, vector_size_t index) {
    // The serialized value is the nullOffset (kNoNullIndex if no null is
    // present) followed by the unique values in order of their indices.
    // The null position is skipped when serializing the values.
    size_t valueSize = sizeof(T);
    size_t totalBytes = kVectorSizeT + (valueSize)*uniqueValues.size();

    auto* flatResult = result->as<FlatVector<StringView>>();
    auto* rawBuffer = flatResult->getRawStringBufferWithSpace(totalBytes, true);
    serializeNullIndex(rawBuffer);

    // nullPosition is set beyond the last position if no null index.
    auto nullPosition = nullIndex.has_value() ? nullIndex : uniqueValues.size();
    size_t offset;
    vector_size_t position;
    for (auto value : uniqueValues) {
      position = value.second;
      // Skip the null position when computing the offset in the serialized
      // buffer.
      offset = kVectorSizeT +
          (position < nullPosition ? position : position - 1) * valueSize;
      memcpy(rawBuffer + offset, &(value.first), valueSize);
    }

    flatResult->setNoCopy(index, StringView(rawBuffer, totalBytes));
  }

  void free(HashStringAllocator& allocator) {
    using UT = decltype(uniqueValues);
    uniqueValues.~UT();
  }

  static const vector_size_t kNoNullIndex = -1;
  static constexpr size_t kVectorSizeT = sizeof(vector_size_t);
};

/// Maintains a set of unique strings.
struct StringViewSetAccumulator {
  /// A set of unique StringViews pointing to storage managed by 'strings'.
  SetAccumulator<StringView> base;

  /// Stores unique non-null non-inline strings.
  Strings strings;

  /// Size (in bytes) of the serialized string values (this includes inline and
  /// non-inline) strings. This value also includes the bytes for serializing
  /// the length value (base.kVectorSizeT) of the strings.
  /// Used for computing serialized buffer size for spilling.
  size_t stringSetBytes = base.kVectorSizeT;

  /// When serializing the strings for spilling, they are written in order of
  /// their indexes. 'offsets' represents the offset of the unique value at that
  /// index from the beginning of the serialization buffer. These offsets are
  /// maintained to easily copy the unique value at that position in the
  /// serialization buffer.
  std::vector<size_t> offsets;

  StringViewSetAccumulator(const TypePtr& type, HashStringAllocator* allocator)
      : base{type, allocator} {}

  void addValue(
      const DecodedVector& decoded,
      vector_size_t index,
      HashStringAllocator* allocator) {
    const auto cnt = base.uniqueValues.size();
    if (decoded.isNullAt(index)) {
      if (!base.nullIndex.has_value()) {
        base.nullIndex = cnt;
        // nullIndex is never encountered in uniqueValues. But we add an entry
        // in the offsets vector to maintain an direct mapping between the
        // index and offsets array.
        offsets.push_back(stringSetBytes);
      }
    } else {
      auto value = decoded.valueAt<StringView>(index);
      addValue(value, base.nullIndex.has_value() ? cnt + 1 : cnt, allocator);
    }
  }

  void addValues(
      const ArrayVector& arrayVector,
      vector_size_t index,
      const DecodedVector& values,
      HashStringAllocator* allocator) {
    const auto size = arrayVector.sizeAt(index);
    const auto offset = arrayVector.offsetAt(index);

    for (auto i = 0; i < size; ++i) {
      addValue(values, offset + i, allocator);
    }
  }

  void deserialize(
      const StringView& serialized,
      HashStringAllocator* allocator) {
    common::InputByteStream stream(serialized.data());
    auto size = serialized.size();

    // The serialized string comprises of nullIndex (or kNoNullIndex)
    // followed by pairs of (length, String value) of the unique
    // values. The unique values are serialized in increasing order of their
    // indexes.
    base.readNullIndex(stream);

    vector_size_t length;
    vector_size_t index = 0;
    while (stream.offset() < size) {
      if (!base.nullIndex.has_value() || index != base.nullIndex.value()) {
        // This is a valid unique value.
        length = stream.read<vector_size_t>();
        addValue(
            StringView(stream.read<char>(length), length), index, allocator);
      }
      index++;
    }
  }

  size_t size() const {
    return base.size();
  }

  vector_size_t extractValues(
      FlatVector<StringView>& values,
      vector_size_t offset) {
    return base.extractValues(values, offset);
  }

  /// Extracts in result[index] a serialized VARBINARY for the String Values.
  /// This is used for the spill of this accumulator.
  void serialize(const VectorPtr& result, vector_size_t index) {
    // nullIndex (or kNoNullIndex) is serialized followed by pairs of
    // (length, String value) of the unique values in the order of their
    // indices.
    auto* flatResult = result->as<FlatVector<StringView>>();
    auto* rawBuffer =
        flatResult->getRawStringBufferWithSpace(stringSetBytes, true);
    base.serializeNullIndex(rawBuffer);

    vector_size_t length;
    char* position;
    // Copy the length and string value at the position from the offsets
    // array. offsets accounts for skipping null index.
    for (const auto& value : base.uniqueValues) {
      position = rawBuffer + offsets[value.second];
      length = value.first.size();
      memcpy(position, &length, base.kVectorSizeT);
      memcpy(position + base.kVectorSizeT, value.first.data(), length);
    }

    flatResult->setNoCopy(index, StringView(rawBuffer, stringSetBytes));
  }

  void free(HashStringAllocator& allocator) {
    strings.free(allocator);
    using Base = decltype(base);
    base.~Base();
  }

 private:
  void addValue(
      const StringView& value,
      vector_size_t index,
      HashStringAllocator* allocator) {
    if (base.uniqueValues.contains(value)) {
      return;
    }
    StringView valueCopy = value;
    if (!valueCopy.isInline()) {
      valueCopy = strings.append(value, *allocator);
    }
    // The new position is written at the end of the serialization buffer.
    offsets.push_back(stringSetBytes);
    base.uniqueValues.insert({valueCopy, index});
    // Accounts for serializing the length of the string as well.
    stringSetBytes += base.kVectorSizeT + valueCopy.size();
  }
};

/// Maintains a set of unique arrays, maps or structs.
struct ComplexTypeSetAccumulator {
  /// A set of pointers to values stored in AddressableNonNullValueList.
  SetAccumulator<
      AddressableNonNullValueList::Entry,
      AddressableNonNullValueList::Hash,
      AddressableNonNullValueList::EqualTo>
      base;

  /// Stores unique non-null values.
  AddressableNonNullValueList values;

  /// Tracks allocated bytes for sizing during serialization for spill.
  /// Initialized to account for the serialization of the null index.
  size_t totalSize = base.kVectorSizeT;

  /// When serializing the values for spilling, they are written in order of
  /// their indexes. 'offsets' represents the offset of the unique value at that
  /// index from the beginning of the serialization buffer. These offsets are
  /// maintained to easily copy the unique value at that position in the
  /// serialization buffer.
  std::vector<size_t> offsets;

  static constexpr size_t kHashSizeT = sizeof(uint64_t);

  ComplexTypeSetAccumulator(const TypePtr& type, HashStringAllocator* allocator)
      : base{
            AddressableNonNullValueList::Hash{},
            AddressableNonNullValueList::EqualTo{type},
            allocator} {}

  void addValue(
      const DecodedVector& decoded,
      vector_size_t i,
      HashStringAllocator* allocator) {
    const auto cnt = base.uniqueValues.size();
    if (decoded.isNullAt(i)) {
      if (!base.nullIndex.has_value()) {
        base.nullIndex = cnt;
        // Adding an entry in the offsets array so that we can maintain
        // a direct mapping of index in the offsets array.
        offsets.push_back(totalSize);
      }
    } else {
      auto entry = values.append(decoded, i, allocator);
      auto index = base.nullIndex.has_value() ? cnt + 1 : cnt;
      if (!base.uniqueValues.insert({entry, index}).second) {
        values.removeLast(entry);
      } else {
        offsets.push_back(totalSize);
        // Accounts for the length of the complex type along with its size and
        // hash.
        totalSize += base.kVectorSizeT + kHashSizeT + entry.size;
      }
    }
  }

  void addValues(
      const ArrayVector& arrayVector,
      vector_size_t index,
      const DecodedVector& values,
      HashStringAllocator* allocator) {
    const auto size = arrayVector.sizeAt(index);
    const auto offset = arrayVector.offsetAt(index);

    for (auto i = 0; i < size; ++i) {
      addValue(values, offset + i, allocator);
    }
  }

  void deserialize(
      const StringView& serialized,
      HashStringAllocator* allocator) {
    auto stream = common::InputByteStream(serialized.data());

    // The serialized string contains the null index followed by pairs of
    // (value size, ComplexType value) of all unique values of the
    // accumulator. The values are in the order of increasing indices.
    base.readNullIndex(stream);

    vector_size_t length;
    vector_size_t index = 0;
    uint64_t hash;
    auto size = serialized.size();
    while (stream.offset() < size) {
      if (!base.nullIndex.has_value() || index != base.nullIndex.value()) {
        // This is a valid serialized unique value.
        length = stream.read<vector_size_t>();
        hash = stream.read<uint64_t>();

        auto result = values.appendSerialized(
            StringView(stream.read<char>(length), length), hash, allocator);
        if (!base.uniqueValues.insert({result, index}).second) {
          values.removeLast(result);
        } else {
          offsets.push_back(totalSize);
          totalSize += base.kVectorSizeT + kHashSizeT + result.size;
        }
      }
      index++;
    }
  }

  size_t size() const {
    return base.size();
  }

  vector_size_t extractValues(BaseVector& values, vector_size_t offset) {
    for (const auto& position : base.uniqueValues) {
      AddressableNonNullValueList::read(
          position.first, values, offset + position.second);
    }

    if (base.nullIndex.has_value()) {
      values.setNull(offset + base.nullIndex.value(), true);
    }

    return base.uniqueValues.size() + (base.nullIndex.has_value() ? 1 : 0);
  }

  /// Extracts in result[index] a serialized VARBINARY for the String Values.
  /// This is used for the spill of this accumulator.
  void serialize(const VectorPtr& result, vector_size_t index) {
    // nullIndex is serialized followed by pairs of (value size, value hash,
    // ComplexType value) of all unique values. The unique values are serialized
    // in order of their indices.

    auto* flatResult = result->as<FlatVector<StringView>>();
    auto* rawBuffer = flatResult->getRawStringBufferWithSpace(totalSize, true);
    base.serializeNullIndex(rawBuffer);

    size_t offset;
    for (const auto& value : base.uniqueValues) {
      offset = offsets.at(value.second);

      memcpy(rawBuffer + offset, &value.first.size, base.kVectorSizeT);
      offset += base.kVectorSizeT;

      memcpy(rawBuffer + offset, &value.first.hash, kHashSizeT);
      offset += kHashSizeT;

      AddressableNonNullValueList::copy(value.first, rawBuffer + offset);
    }

    flatResult->setNoCopy(index, StringView(rawBuffer, totalSize));
  }

  void free(HashStringAllocator& allocator) {
    values.free(allocator);
    using Base = decltype(base);
    base.~Base();
  }
};

template <typename T>
struct SetAccumulatorTypeTraits {
  using AccumulatorType = SetAccumulator<T>;
};

template <>
struct SetAccumulatorTypeTraits<StringView> {
  using AccumulatorType = StringViewSetAccumulator;
};

template <>
struct SetAccumulatorTypeTraits<ComplexType> {
  using AccumulatorType = ComplexTypeSetAccumulator;
};
} // namespace detail

template <typename T>
using SetAccumulator =
    typename detail::SetAccumulatorTypeTraits<T>::AccumulatorType;

} // namespace facebook::velox::aggregate::prestosql
