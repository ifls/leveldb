// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/log_writer.h"

#include <cstdint>

#include "leveldb/env.h"

#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {
namespace log {

static void InitTypeCrc(uint32_t *type_crc) {
	for (int i = 0; i <= kMaxRecordType; i++) {
		char t = static_cast<char>(i);
		type_crc[i] = crc32c::Value(&t, 1);
	}
}

Writer::Writer(WritableFile *dest) : dest_(dest), block_offset_(0) {
	InitTypeCrc(type_crc_);
}

Writer::Writer(WritableFile *dest, uint64_t dest_length)
	: dest_(dest), block_offset_(dest_length % kBlockSize) {
	InitTypeCrc(type_crc_);
}

Writer::~Writer() = default;

// 记录 写操作， 更新操作
Status Writer::AddRecord(const Slice &slice) {
	const char *ptr = slice.data();
	size_t left = slice.size();

	// Fragment the record if necessary and emit it.  Note that if slice
	// is empty, we still want to iterate once to emit a single
	// zero-length record
	Status s;
	bool begin = true;  //是日志开头
	do {
		const int leftover = kBlockSize - block_offset_;  //当前块剩余空间
		assert(leftover >= 0);
		if (leftover < kHeaderSize) {  //连头部7B 都放不下
			// Switch to a new block
			if (leftover > 0) {
				// Fill the trailer (literal below relies on kHeaderSize being 7)
				static_assert(kHeaderSize == 7, "");
				// 只能追加 0值，填满剩下的
				dest_->Append(Slice("\x00\x00\x00\x00\x00\x00", leftover));
			}
			block_offset_ = 0;  //新块的开头
		}

		// Invariant: we never leave < kHeaderSize bytes in a block.
		assert(kBlockSize - block_offset_ - kHeaderSize >= 0);

		// 如果 恰好是7B 会怎么样，会存一个长度为0的头部  crc4B, 0x00002B, type1B
		const size_t avail = kBlockSize - block_offset_ - kHeaderSize;  // 除头部外的剩余空间
		const size_t fragment_length = (left < avail) ? left : avail;  //要占用的空间

		RecordType type;
		const bool end = (left == fragment_length);  // 空间够的情况 left < avail
		if (begin && end) {                          //
			type = kFullType;
		} else if (begin) {
			type = kFirstType;
		} else if (end) {
			type = kLastType;
		} else {
			type = kMiddleType;
		}
		//提交物理日志记录
		s = EmitPhysicalRecord(type, ptr, fragment_length);
		ptr += fragment_length;
		left -= fragment_length;
		begin = false;  //之后就不是日志开头了
	} while (s.ok() && left > 0);
	return s;
}

//提交物理日志
Status Writer::EmitPhysicalRecord(RecordType t, const char *ptr, size_t length) {
	assert(length <= 0xffff);  // Must fit in two bytes
	assert(block_offset_ + kHeaderSize + length <= kBlockSize);

	// Format the header
	char buf[kHeaderSize];
	//4B checksum
	//2B长度 小端顺序
	buf[4] = static_cast<char>(length & 0xff);
	buf[5] = static_cast<char>(length >> 8);
	//1B类型
	buf[6] = static_cast<char>(t);

	// Compute the crc of the record type and the payload.
	uint32_t crc = crc32c::Extend(type_crc_[t], ptr, length);
	crc = crc32c::Mask(crc);  // Adjust for storage
	EncodeFixed32(buf, crc);  //crc -> 4B

	// Write the header and the payload
	// 写入头部到日志文件
	Status s = dest_->Append(Slice(buf, kHeaderSize));
	if (s.ok()) {
		//写入body
		s = dest_->Append(Slice(ptr, length));
		if (s.ok()) {
			//不刷新到磁盘， 只是刷新到内核缓冲区, 调用方根据sync选项调用文件的sync函数，刷到磁盘
			s = dest_->Flush();
		}
	}
	// 如果日志出错，依然前进这么多字节
	block_offset_ += kHeaderSize + length;
	return s;
}

}  // namespace log
}  // namespace leveldb
