// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A Cache is an interface接口 that maps keys to values.
// It has internal synchronization and may be safely accessed concurrently from multiple threads. 多线程安全
// It may automatically evict自动驱逐 entries to make room整理空间 for new entries.
// Values have a specified charge消耗 against the cache capacity.
// For example, a cache where the values are variable length strings, may use the length of the string as the charge for
// the string.
//
// A builtin cache implementation with a least-recently-used eviction policy is provided.  ShardedLRUCache
// Clients may use their own implementations if
// they want something more sophisticated (like scan-resistance(抗扫描，不会因为扫描而导致缓存被大量替换), a
// custom eviction policy, variable cache sizing, etc.)

#ifndef STORAGE_LEVELDB_INCLUDE_CACHE_H_
#define STORAGE_LEVELDB_INCLUDE_CACHE_H_

#include <cstdint>

#include "leveldb/export.h"
#include "leveldb/slice.h"

namespace leveldb {

	class LEVELDB_EXPORT Cache;

// Create a new cache with a fixed size capacity.  This implementation
// of Cache uses a least-recently-used eviction policy.
	LEVELDB_EXPORT Cache *NewLRUCache(size_t capacity);

	// 缓存的通用接口
	// 实现类 ShardedLRUCache
	class LEVELDB_EXPORT Cache {
	public:
		Cache() = default;  // 默认构造函数

		Cache(const Cache &) = delete;

		Cache &operator=(const Cache &) = delete;

		// Destroys all existing entries by calling the "deleter"
		// function that was passed to the constructor.
		virtual ~Cache();

		// Opaque handle to an entry stored in the cache.
		struct Handle {
		};        //充当一个具体类型的指针

		// Insert a mapping from key->value into the cache and
		// assign it the specified charge(用于表示本次插入操作对cache容量的消耗) against the total cache capacity.
		//
		// Returns a handle that corresponds to the mapping.
		// The caller must call this->Release(handle) when the returned mapping is no longer needed.
		//
		// When the inserted entry is no longer needed, the key and value will be passed to "deleter". 回调
		virtual Handle *
		Insert(const Slice &key, void *value, size_t charge, void (*deleter)(const Slice &key, void *value)
		) = 0;

		// If the cache has no mapping for "key", returns nullptr.
		//
		// Else return a handle that corresponds to the mapping.  The caller
		// must call this->Release(handle) when the returned mapping is no
		// longer needed.
		virtual Handle *Lookup(const Slice &key) = 0;

		// Release a mapping returned by a previous Lookup().
		// REQUIRES: handle must not have been released yet.
		// REQUIRES: handle must have been returned by a method on *this.
		virtual void Release(Handle *handle) = 0;

		// Return the value encapsulated in a handle returned by a successful Lookup().
		// 从handle中获取value
		// REQUIRES: handle must not have been released yet.
		// REQUIRES: handle must have been returned by a method on *this.
		virtual void *Value(Handle *handle) = 0;

		// If the cache contains entry for key, erase it.  擦除和释放的区别
		// Note that the underlying entry will be kept around until all existing handles to it have been released.
		virtual void Erase(const Slice &key) = 0;

		// Return a new numeric id.
		// May be used by multiple clients who are sharing the same cache to partition the key space.  划分key命名空间
		// Typically the client will allocate a new id at startup and prepend追加 the id to its cache keys.
		virtual uint64_t NewId() = 0;

		// Remove all cache entries that are not actively in use.  移除所有未在使用的条目
		// Memory-constrained内存受限 applications may wish to call this method to reduce memory usage.
		// Default implementation of Prune() does nothing.
		// Subclasses are strongly encouraged to override the default implementation.
		// A future release of leveldb may change Prune() to a pure abstract method.
		virtual void Prune() {}

		// Return an estimate of the combined charges of all elements stored in the cache.
		virtual size_t TotalCharge() const = 0;  //总消耗

	private:
		void LRU_Remove(Handle *e);

		void LRU_Append(Handle *e);

		void Unref(Handle *e);

		struct Rep;
		Rep *rep_;
	};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_CACHE_H_
