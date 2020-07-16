// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_LOG_WRITER_H_
#define STORAGE_LEVELDB_DB_LOG_WRITER_H_

#include <cstdint>

#include "db/log_format.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

class WritableFile;

namespace log {

// wal日志的实现和封装
class Writer {
 public:
  // Create a writer that will append data to "*dest".
  // "*dest" must be initially empty. 文件开始必须是空的
  // "*dest" must remain live while this Writer is in use. 文件不能提前释放or关闭
  explicit Writer(WritableFile *dest);

  // Create a writer that will append data to "*dest".
  // "*dest" must have initial length "dest_length". 一开始必须有足够长度的空间
  // "*dest" must remain live while this Writer is in use.
  Writer(WritableFile *dest, uint64_t dest_length);

  Writer(const Writer &) = delete;

  Writer &operator=(const Writer &) = delete;

  ~Writer();

  // 就两个函数， 一个公有函数
  Status AddRecord(const Slice &slice);

 private:
  //内部函数
  Status EmitPhysicalRecord(RecordType type, const char *ptr, size_t length);

  WritableFile *dest_;
  int block_offset_;  // 当前块中的偏移 Current offset in block

  // crc32c values for all supported record types.  These are
  // pre-computed to reduce the overhead间接成本 of computing the crc of the
  // record type stored in the header.
  uint32_t type_crc_[kMaxRecordType + 1];  //不同类型的crc码不同
};

}  // namespace log
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_LOG_WRITER_H_
