// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Log format information shared by reader and writer.
// See ../doc/log_format.md for more detail.

#ifndef STORAGE_LEVELDB_DB_LOG_FORMAT_H_
#define STORAGE_LEVELDB_DB_LOG_FORMAT_H_

namespace leveldb {
	namespace log {

		enum RecordType {
			// Zero is reserved for preallocated files
			kZeroType = 0,

			kFullType = 1,  // 一条日志完整地在一块上

			// For fragments
			kFirstType = 2,   //一条日志的 开头部分在此块上
			kMiddleType = 3,  //一条日志的中间部分在此块上
			kLastType = 4  //一条日志的 结尾部分在此块上
		};

// 用于追加定义之后的枚举
		static const int kMaxRecordType = kLastType;

//一个日志文件， 被分隔成多个物理块 32K
		static const int kBlockSize = 32768;

// Header is checksum (4 bytes), length (2 bytes 2^16), type (1 byte). 就是上面的record_type
		static const int kHeaderSize = 4 + 2 + 1;

	}  // namespace log
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_LOG_FORMAT_H_
