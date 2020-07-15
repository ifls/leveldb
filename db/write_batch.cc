// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
// writeBatch 内部string的格式
// WriteBatch::rep_ :=
//    sequence: fixed64
//    count: fixed32                kv数量 偏移8B
//    data: record[count]
// record :=
//    kTypeValue varstring varstring         |
//    kTypeDeletion varstring
// varstring :=
//    len: varint32
//    data: uint8[len]

#include "leveldb/write_batch.h"

#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "leveldb/db.h"
#include "util/coding.h"

namespace leveldb {

// WriteBatch header has an 8-byte sequence number followed by a 4-byte count.
	static const size_t kHeader = 12;

	WriteBatch::WriteBatch() { Clear(); }

	WriteBatch::~WriteBatch() = default;

	WriteBatch::Handler::~Handler() = default;

	void WriteBatch::Clear() {
		rep_.clear();
		rep_.resize(kHeader);
	}

	size_t WriteBatch::ApproximateSize() const { return rep_.size(); }

	Status WriteBatch::Iterate(Handler *handler) const {
		Slice input(rep_);
		if (input.size() < kHeader) {   // 头部都没有，算成是错误数据
			return Status::Corruption("malformed WriteBatch (too small)");
		}

		input.remove_prefix(kHeader);   // 之后是重复的 record[n] 数组
		Slice key, value;
		int found = 0;
		//循环写入 每个entry
		while (!input.empty()) {
			found++;
			//第一字节是类型
			char tag = input[0];
			input.remove_prefix(1);
			switch (tag) {
				case kTypeValue:
					if (GetLengthPrefixedSlice(&input, &key) &&
						GetLengthPrefixedSlice(&input, &value)) {
						//拿到kv，插入到内存表
						handler->Put(key, value);
					} else {
						return Status::Corruption("bad WriteBatch Put");
					}
					break;
				case kTypeDeletion:
					if (GetLengthPrefixedSlice(&input, &key)) {
						//删除 内存表 key
						handler->Delete(key);
					} else {
						return Status::Corruption("bad WriteBatch Delete");
					}
					break;
				default:
					return Status::Corruption("unknown WriteBatch tag");
			}
		}

		//读到的数量和存放的数量对不上
		if (found != WriteBatchInternal::Count(this)) {
			return Status::Corruption("WriteBatch has wrong count");
		} else {
			return Status::OK();
		}
	}

	int WriteBatchInternal::Count(const WriteBatch *b) {
		return DecodeFixed32(b->rep_.data() + 8);
	}

	void WriteBatchInternal::SetCount(WriteBatch *b, int n) {
		EncodeFixed32(&b->rep_[8], n);
	}

	SequenceNumber WriteBatchInternal::Sequence(const WriteBatch *b) {
		// 前8B是 sequence
		return SequenceNumber(DecodeFixed64(b->rep_.data()));
	}

	void WriteBatchInternal::SetSequence(WriteBatch *b, SequenceNumber seq) {
		EncodeFixed64(&b->rep_[0], seq);
	}

	void WriteBatch::Put(const Slice &key, const Slice &value) {
		WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
		// type keystring valstring
		rep_.push_back(static_cast<char>(kTypeValue));
		PutLengthPrefixedSlice(&rep_, key);         //长度和数据
		PutLengthPrefixedSlice(&rep_, value);
	}

	void WriteBatch::Delete(const Slice &key) {
		WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
		// type keystring  没有valstring
		rep_.push_back(static_cast<char>(kTypeDeletion));
		PutLengthPrefixedSlice(&rep_, key);
	}

	void WriteBatch::Append(const WriteBatch &source) {
		WriteBatchInternal::Append(this, &source);
	}

	namespace {
		class MemTableInserter : public WriteBatch::Handler {
		public:
			SequenceNumber sequence_;       //序号
			MemTable *mem_;

			//负责填充序号和type
			void Put(const Slice &key, const Slice &value) override {
				mem_->Add(sequence_, kTypeValue, key, value);
				sequence_++;
			}

			void Delete(const Slice &key) override {
				mem_->Add(sequence_, kTypeDeletion, key, Slice());
				sequence_++;
			}
		};
	}  // namespace

	// 插入到内存表
	Status WriteBatchInternal::InsertInto(const WriteBatch *b, MemTable *memtable) {
		MemTableInserter inserter;
		inserter.sequence_ = WriteBatchInternal::Sequence(b); //从writebatch里拿到序号
		inserter.mem_ = memtable;
		return b->Iterate(&inserter);
	}

	void WriteBatchInternal::SetContents(WriteBatch *b, const Slice &contents) {
		assert(contents.size() >= kHeader);
		b->rep_.assign(contents.data(), contents.size());
	}

	void WriteBatchInternal::Append(WriteBatch *dst, const WriteBatch *src) {
		SetCount(dst, Count(dst) + Count(src));
		assert(src->rep_.size() >= kHeader);
		// 8B 序号 4B 数量
		dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
	}

}  // namespace leveldb
