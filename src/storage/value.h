// tagged union over every Redis data type gredis stores
#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <variant>

#include "storage/hash_table.h"
#include "storage/zset.h"

namespace gredis {

// Redis "hash": field -> value, both strings, reuses the keyspace's HashTable
using HashValue = HashTable<std::string, std::string>;

// Redis "list": plain deque, O(1) push/pop both ends is all it needs
using ListValue = std::deque<std::string>;

// zero-size marker for HashTable's value type when used as a set
struct Empty {};

// Redis "set": same HashTable as HashValue, empty value type
using SetValue = HashTable<std::string, Empty>;

// unique_ptr for the aggregates so a plain string Value doesn't pay for
// their size, and so the aggregate has a stable address while owned
using Value = std::variant<std::string, std::unique_ptr<HashValue>, std::unique_ptr<ListValue>,
                            std::unique_ptr<SetValue>, std::unique_ptr<ZSet>>;

// "string"/"hash"/"list"/"set"/"zset", for the TYPE command
const char* type_name(const Value& v);

// element count, for UNLINK's background-vs-inline threshold (always 1
// for a string, size() for the aggregates)
size_t value_element_count(const Value& v);

} // namespace gredis
