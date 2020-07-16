// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_VERSION_EDIT_H_
#define STORAGE_LEVELDB_DB_VERSION_EDIT_H_

#include <set>
#include <utility>
#include <vector>

#include "db/dbformat.h"

namespace leveldb {

	class VersionSet;

	struct FileMetaData {
		FileMetaData() : refs(0), allowed_seeks(1 << 30), file_size(0) {}

		int refs;   // 内存引用计数
		int allowed_seeks;  // 允许查找多少次 Seeks allowed until compaction
		uint64_t number;    // sst文件 序号
		uint64_t file_size;    // 文件大小 File size in bytes
		InternalKey smallest;  // 最小键 Smallest internal key served by table
		InternalKey largest;   // 最大键 Largest internal key served by table
	};

	// 每次 sst 变动, 要生成这个类, 执行 VersionSet::LogAndApply, 数据要么在日志中，要么在sst中，才能保证不丢失
	class VersionEdit {
	public:
		VersionEdit() { Clear(); }

		~VersionEdit() = default;

		void Clear();

		void SetComparatorName(const Slice &name) {
			has_comparator_ = true;
			comparator_ = name.ToString();
		}

		void SetLogNumber(uint64_t num) {
			has_log_number_ = true;
			log_number_ = num;
		}

		void SetPrevLogNumber(uint64_t num) {
			has_prev_log_number_ = true;
			prev_log_number_ = num;
		}

		void SetNextFile(uint64_t num) {
			has_next_file_number_ = true;
			next_file_number_ = num;
		}

		void SetLastSequence(SequenceNumber seq) {
			has_last_sequence_ = true;
			last_sequence_ = seq;
		}

		void SetCompactPointer(int level, const InternalKey &key) {
			compact_pointers_.push_back(std::make_pair(level, key));
		}

		// Add the specified file at the specified number. 增加新文件
		// REQUIRES: This version has not been saved (see VersionSet::SaveTo)
		// REQUIRES: "smallest" and "largest" are smallest and largest keys in file
		void
		AddFile(int level, uint64_t file, uint64_t file_size, const InternalKey &smallest, const InternalKey &largest) {
			FileMetaData f;
			f.number = file;   // 文件序号
			f.file_size = file_size;
			f.smallest = smallest;
			f.largest = largest;
			//加到集合
			new_files_.push_back(std::make_pair(level, f));
		}

		// Delete the specified "file" from the specified "level".
		void RemoveFile(int level, uint64_t file) {
			deleted_files_.insert(std::make_pair(level, file));
		}

		void EncodeTo(std::string *dst) const;

		Status DecodeFrom(const Slice &src);

		std::string DebugString() const;

	private:
		friend class VersionSet;

		typedef std::set<std::pair<int, uint64_t>> DeletedFileSet;

		std::string comparator_;
		uint64_t log_number_;
		uint64_t prev_log_number_;   // 变更前的日志序号
		uint64_t next_file_number_;  //文件序号
		SequenceNumber last_sequence_;  // 修改序号

		// 与上面5个对应
		bool has_comparator_;
		bool has_log_number_;
		bool has_prev_log_number_;
		bool has_next_file_number_;
		bool has_last_sequence_;

		// 这是什么
		std::vector<std::pair<int, InternalKey>> compact_pointers_;     // <int> 标识层级 0层，1层
		// 相关文件集合
		DeletedFileSet deleted_files_;  //待删除的文件
		std::vector<std::pair<int, FileMetaData>> new_files_;  //新增文件， imm -> l0 就会添加的这里
	};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_VERSION_EDIT_H_
