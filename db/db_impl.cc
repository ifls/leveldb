// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_impl.h"

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/status.h"
#include "leveldb/table.h"
#include "leveldb/table_builder.h"

#include "port/port.h"
#include "table/block.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/mutexlock.h"

namespace leveldb {

	const int kNumNonTableCacheFiles = 10;

// Information kept for every waiting writer
	struct DBImpl::Writer {
		explicit Writer(port::Mutex *mu)
				: batch(nullptr), sync(false), done(false), cv(mu) {}

		Status status;  //记录写操作的结果
		WriteBatch *batch; // 保存写操作
		bool sync;   //根据配置决定是否立即同步到磁盘
		bool done;  //是否已经写完成
		port::CondVar cv;  //条件变量，用于阻塞排队
	};

	struct DBImpl::CompactionState {
		// Files produced by compaction
		struct Output {
			uint64_t number;
			uint64_t file_size;
			InternalKey smallest, largest;
		};

		Output *current_output() { return &outputs[outputs.size() - 1]; }

		explicit CompactionState(Compaction *c)
				: compaction(c),
				  smallest_snapshot(0),
				  outfile(nullptr),
				  builder(nullptr),
				  total_bytes(0) {}

		Compaction *const compaction;

		// Sequence numbers < smallest_snapshot are not significant since we
		// will never have to service a snapshot below smallest_snapshot.
		// Therefore if we have seen a sequence number S <= smallest_snapshot,
		// we can drop all entries for the same key with sequence numbers < S.
		SequenceNumber smallest_snapshot;

		std::vector<Output> outputs;

		// State kept for output being generated
		WritableFile *outfile;
		TableBuilder *builder;

		uint64_t total_bytes;
	};

// Fix user-supplied options to be reasonable
	template<class T, class V>
	static void ClipToRange(T *ptr, V minvalue, V maxvalue) {
		if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
		if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
	}

	Options SanitizeOptions(const std::string &dbname,
							const InternalKeyComparator *icmp,
							const InternalFilterPolicy *ipolicy,
							const Options &src) {
		Options result = src;
		result.comparator = icmp;
		result.filter_policy = (src.filter_policy != nullptr) ? ipolicy : nullptr;
		ClipToRange(&result.max_open_files, 64 + kNumNonTableCacheFiles, 50000);
		ClipToRange(&result.write_buffer_size, 64 << 10, 1 << 30);
		ClipToRange(&result.max_file_size, 1 << 20, 1 << 30);
		ClipToRange(&result.block_size, 1 << 10, 4 << 20);
		if (result.info_log == nullptr) {
			// Open a log file in the same directory as the db
			src.env->CreateDir(dbname);  // In case it does not exist
			src.env->RenameFile(InfoLogFileName(dbname), OldInfoLogFileName(dbname));
			Status s = src.env->NewLogger(InfoLogFileName(dbname), &result.info_log);
			if (!s.ok()) {
				// No place suitable for logging
				result.info_log = nullptr;
			}
		}
		if (result.block_cache == nullptr) {
			result.block_cache = NewLRUCache(8 << 20);
		}
		return result;
	}

	static int TableCacheSize(const Options &sanitized_options) {
		// Reserve ten files or so for other uses and give the rest to TableCache.
		return sanitized_options.max_open_files - kNumNonTableCacheFiles;
	}

	DBImpl::DBImpl(const Options &raw_options, const std::string &dbname)
			: env_(raw_options.env),
			  internal_comparator_(raw_options.comparator),
			  internal_filter_policy_(raw_options.filter_policy),
			  options_(SanitizeOptions(dbname, &internal_comparator_,
									   &internal_filter_policy_, raw_options)),
			  owns_info_log_(options_.info_log != raw_options.info_log),
			  owns_cache_(options_.block_cache != raw_options.block_cache),
			  dbname_(dbname),
			  table_cache_(new TableCache(dbname_, options_, TableCacheSize(options_))),
			  db_lock_(nullptr),
			  shutting_down_(false),
			  background_work_finished_signal_(&mutex_),
			  mem_(nullptr),
			  imm_(nullptr),
			  has_imm_(false),
			  logfile_(nullptr),
			  logfile_number_(0),
			  log_(nullptr),
			  seed_(0),
			  tmp_batch_(new WriteBatch),
			  background_compaction_scheduled_(false),
			  manual_compaction_(nullptr),
			  versions_(new VersionSet(dbname_, &options_, table_cache_,
									   &internal_comparator_)) {}

	DBImpl::~DBImpl() {
		// Wait for background work to finish.
		mutex_.Lock();
		shutting_down_.store(true, std::memory_order_release);
		while (background_compaction_scheduled_) {  // 等待压缩完成
			background_work_finished_signal_.Wait();
		}
		mutex_.Unlock();

		if (db_lock_ != nullptr) {
			env_->UnlockFile(db_lock_);
		}

		delete versions_;
		if (mem_ != nullptr) mem_->Unref();
		if (imm_ != nullptr) imm_->Unref();
		delete tmp_batch_;
		delete log_;
		delete logfile_;
		delete table_cache_;

		if (owns_info_log_) {
			delete options_.info_log;
		}
		if (owns_cache_) {
			delete options_.block_cache;
		}
	}

	Status DBImpl::NewDB() {
		VersionEdit new_db;
		new_db.SetComparatorName(user_comparator()->Name());
		new_db.SetLogNumber(0);
		new_db.SetNextFile(2); // manifest 文件 占用了序号1, 后面从2开始
		new_db.SetLastSequence(0);

		const std::string manifest = DescriptorFileName(dbname_, 1);  // manifest 文件名
		WritableFile *file;
		Status s = env_->NewWritableFile(manifest, &file);  // 创建manifest文件
		if (!s.ok()) {
			return s;
		}

		{
			log::Writer log(file);
			std::string record;

			new_db.EncodeTo(&record); // version edit 转换为 日志
			s = log.AddRecord(record);  // 写入到manifest 日志文件
			if (s.ok()) {
				s = file->Close();  // 为什么要直接关闭?
			}
		}
		delete file;

		if (s.ok()) {
			// Make "CURRENT" file that points to the new manifest file.
			s = SetCurrentFile(env_, dbname_, 1);  // 记录当前manifest文件的序号
		} else {
			env_->RemoveFile(manifest);
		}
		return s;
	}

	void DBImpl::MaybeIgnoreError(Status *s) const {
		if (s->ok() || options_.paranoid_checks) {
			// No change needed
		} else {
			Log(options_.info_log, "Ignoring error %s", s->ToString().c_str());
			*s = Status::OK();
		}
	}

	// 删除不需要的文件
	void DBImpl::RemoveObsoleteFiles() {
		mutex_.AssertHeld();

		if (!bg_error_.ok()) {
			// After a background error, we don't know whether a new version may
			// or may not have been committed, so we cannot safely garbage collect.
			return;
		}

		// Make a set of all of the live files
		std::set<uint64_t> live = pending_outputs_;
		versions_->AddLiveFiles(&live);

		std::vector<std::string> filenames;
		env_->GetChildren(dbname_, &filenames);  // Ignoring errors on purpose
		uint64_t number;
		FileType type;

		std::vector<std::string> files_to_delete;

		for (std::string &filename : filenames) {
			// 读出 number 和 type
			if (ParseFileName(filename, &number, &type)) {
				bool keep = true;
				switch (type) {
					case kLogFile:  // >= prev log number 就保留
						keep = ((number >= versions_->LogNumber()) || (number == versions_->PrevLogNumber()));
						break;
					case kDescriptorFile:
						// Keep my manifest file, and any newer incarnations'
						// (in case there is a race that allows other incarnations)
						keep = (number >= versions_->ManifestFileNumber());
						break;
					case kTableFile:
						keep = (live.find(number) != live.end());  // 还在被当前的version使用
						break;
					case kTempFile:
						// Any temp files that are currently being written to must
						// be recorded in pending_outputs_, which is inserted into "live"
						keep = (live.find(number) != live.end());
						break;
					case kCurrentFile: // current 和 lock文件要删除
					case kDBLockFile:
					case kInfoLogFile:
						keep = true;
						break;
				}

				if (!keep) {
					files_to_delete.push_back(std::move(filename));
					if (type == kTableFile) {
						table_cache_->Evict(number);  //移除sst文件, 清缓存
					}
					Log(options_.info_log, "Delete type=%d #%lld\n", static_cast<int>(type),
						static_cast<unsigned long long>(number));
				}
			}
		}

		// While deleting all files unblock other threads. All files being deleted
		// have unique names which will not collide with newly created files and
		// are therefore safe to delete while allowing other threads to proceed.
		mutex_.Unlock();
		for (const std::string &filename : files_to_delete) {
			// 删除文件
			env_->RemoveFile(dbname_ + "/" + filename);
		}
		mutex_.Lock();
	}

	Status DBImpl::Recover(VersionEdit *edit, bool *save_manifest) {
		mutex_.AssertHeld();

		// Ignore error from CreateDir since the creation of the DB is
		// committed only when the descriptor is created, and this directory
		// may already exist from a previous failed creation attempt.
		env_->CreateDir(dbname_);

		assert(db_lock_ == nullptr);

		Status s = env_->LockFile(LockFileName(dbname_), &db_lock_); // 文件锁 上锁
		if (!s.ok()) {
			return s;
		}

		if (!env_->FileExists(CurrentFileName(dbname_))) {
			// 没有current文件, 表示没数据库?
			if (options_.create_if_missing) {
				s = NewDB();
				if (!s.ok()) {
					return s;
				}
			} else {
				return Status::InvalidArgument(dbname_, "does not exist (create_if_missing is false)");
			}
		} else {
			if (options_.error_if_exists) {
				return Status::InvalidArgument(dbname_, "exists (error_if_exists is true)");
			}
		}

		// 恢复 sst元数据
		s = versions_->Recover(save_manifest);
		if (!s.ok()) {
			return s;
		}
		SequenceNumber max_sequence(0);

		// Recover from all newer log files than the ones named in the
		// descriptor (new log files may have been added by the previous
		// incarnation without registering them in the descriptor).
		//
		// Note that PrevLogNumber() is no longer used, but we pay
		// attention to it in case we are recovering a database
		// produced by an older version of leveldb.
		const uint64_t min_log = versions_->LogNumber();
		const uint64_t prev_log = versions_->PrevLogNumber();
		std::vector<std::string> filenames;
		s = env_->GetChildren(dbname_, &filenames);
		if (!s.ok()) {
			return s;
		}
		std::set<uint64_t> expected;
		versions_->AddLiveFiles(&expected);
		uint64_t number;
		FileType type;
		std::vector<uint64_t> logs;  // 收集需要恢复的数据日志文件 序号
		for (size_t i = 0; i < filenames.size(); i++) {
			if (ParseFileName(filenames[i], &number, &type)) {
				expected.erase(number);
				if (type == kLogFile && ((number >= min_log) || (number == prev_log)))
					logs.push_back(number);
			}
		}
		if (!expected.empty()) {
			char buf[50];
			std::snprintf(buf, sizeof(buf), "%d missing files; e.g.",
						  static_cast<int>(expected.size()));
			return Status::Corruption(buf, TableFileName(dbname_, *(expected.begin())));
		}

		// Recover in the order in which the logs were generated
		std::sort(logs.begin(), logs.end());
		for (size_t i = 0; i < logs.size(); i++) {
			// 从log文件 恢复 memtable
			s = RecoverLogFile(logs[i], (i == logs.size() - 1), save_manifest, edit, &max_sequence);
			if (!s.ok()) {
				return s;
			}

			// The previous incarnation may not have written any MANIFEST
			// records after allocating this log number.  So we manually
			// update the file number allocation counter in VersionSet.
			versions_->MarkFileNumberUsed(logs[i]);  // 更新最大文件序号
		}

		if (versions_->LastSequence() < max_sequence) {
			versions_->SetLastSequence(max_sequence);  // 更新 最大 修改序号
		}

		return Status::OK();
	}

	Status DBImpl::RecoverLogFile(uint64_t log_number, bool last_log,
								  bool *save_manifest, VersionEdit *edit,
								  SequenceNumber *max_sequence) {
		struct LogReporter : public log::Reader::Reporter {
			Env *env;
			Logger *info_log;
			const char *fname;
			Status *status;  // null if options_.paranoid_checks==false
			void Corruption(size_t bytes, const Status &s) override {
				Log(info_log, "%s%s: dropping %d bytes; %s",
					(this->status == nullptr ? "(ignoring error) " : ""), fname,
					static_cast<int>(bytes), s.ToString().c_str());
				if (this->status != nullptr && this->status->ok()) *this->status = s;
			}
		};

		mutex_.AssertHeld();

		// Open the log file
		std::string fname = LogFileName(dbname_, log_number);
		SequentialFile *file;
		Status status = env_->NewSequentialFile(fname, &file);
		if (!status.ok()) {
			MaybeIgnoreError(&status);
			return status;
		}

		// Create the log reader.
		LogReporter reporter;
		reporter.env = env_;
		reporter.info_log = options_.info_log;
		reporter.fname = fname.c_str();
		reporter.status = (options_.paranoid_checks ? &status : nullptr);
		// We intentionally make log::Reader do checksumming even if
		// paranoid_checks==false so that corruptions cause entire commits
		// to be skipped instead of propagating bad information (like overly
		// large sequence numbers).
		log::Reader reader(file, &reporter, true /*checksum*/, 0 /*initial_offset*/);
		Log(options_.info_log, "Recovering log #%llu",
			(unsigned long long) log_number);

		// Read all the records and add to a memtable
		std::string scratch;
		Slice record;
		WriteBatch batch;
		int compactions = 0;
		MemTable *mem = nullptr;
		while (reader.ReadRecord(&record, &scratch) && status.ok()) {
			if (record.size() < 12) {
				reporter.Corruption(record.size(),
									Status::Corruption("log record too small"));
				continue;
			}
			WriteBatchInternal::SetContents(&batch, record);

			if (mem == nullptr) {
				mem = new MemTable(internal_comparator_);
				mem->Ref();
			}
			status = WriteBatchInternal::InsertInto(&batch, mem);
			MaybeIgnoreError(&status);
			if (!status.ok()) {
				break;
			}
			const SequenceNumber last_seq = WriteBatchInternal::Sequence(&batch) +
											WriteBatchInternal::Count(&batch) - 1;
			if (last_seq > *max_sequence) {
				*max_sequence = last_seq;
			}

			if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
				compactions++;
				*save_manifest = true;
				status = WriteLevel0Table(mem, edit, nullptr);
				mem->Unref();
				mem = nullptr;
				if (!status.ok()) {
					// Reflect errors immediately so that conditions like full
					// file-systems cause the DB::Open() to fail.
					break;
				}
			}
		}

		delete file;

		// See if we should keep reusing the last log file.
		if (status.ok() && options_.reuse_logs && last_log && compactions == 0) {
			assert(logfile_ == nullptr);
			assert(log_ == nullptr);
			assert(mem_ == nullptr);
			uint64_t lfile_size;
			if (env_->GetFileSize(fname, &lfile_size).ok() &&
				env_->NewAppendableFile(fname, &logfile_).ok()) {
				Log(options_.info_log, "Reusing old log %s \n", fname.c_str());
				log_ = new log::Writer(logfile_, lfile_size);
				logfile_number_ = log_number;
				if (mem != nullptr) {
					mem_ = mem;
					mem = nullptr;
				} else {
					// mem can be nullptr if lognum exists but was empty.
					mem_ = new MemTable(internal_comparator_);
					mem_->Ref();
				}
			}
		}

		if (mem != nullptr) {
			// mem did not get reused; compact it.
			if (status.ok()) {
				*save_manifest = true;
				status = WriteLevel0Table(mem, edit, nullptr);
			}
			mem->Unref();
		}

		return status;
	}

	// imm -> l0
	Status DBImpl::WriteLevel0Table(MemTable *mem, VersionEdit *edit, Version *base) {
		mutex_.AssertHeld();
		const uint64_t start_micros = env_->NowMicros();
		FileMetaData meta;
		meta.number = versions_->NewFileNumber();  //分配新的文件序号

		pending_outputs_.insert(meta.number);

		Iterator *iter = mem->NewIterator();
		Log(options_.info_log, "Level-0 table #%llu: started", (unsigned long long) meta.number);

		Status s;
		{
			mutex_.Unlock();
			//  生成并写入 sst
			s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
			mutex_.Lock();
		}

		Log(options_.info_log, "Level-0 table #%llu: %lld bytes %s", (unsigned long long) meta.number,
			(unsigned long long) meta.file_size, s.ToString().c_str());
		delete iter;
		pending_outputs_.erase(meta.number);

		// Note that if file_size is zero, the file has been deleted and
		// should not be added to the manifest.
		int level = 0;
		if (s.ok() && meta.file_size > 0) {
			const Slice min_user_key = meta.smallest.user_key();
			const Slice max_user_key = meta.largest.user_key();
			if (base != nullptr) {
				// 根据 最小key 和 最大key 计算新生成的sst 应该在哪个level
				level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
			}
			// 插入指定level,保存sst元数据
			edit->AddFile(level, meta.number, meta.file_size, meta.smallest, meta.largest);
		}

		// 更新统计数据
		CompactionStats stats;
		stats.micros = env_->NowMicros() - start_micros;
		stats.bytes_written = meta.file_size;
		stats_[level].Add(stats);
		return s;
	}

	void DBImpl::CompactMemTable() {
		mutex_.AssertHeld();
		assert(imm_ != nullptr);

		// Save the contents of the memtable as a new Table
		VersionEdit edit;
		Version *base = versions_->current();
		base->Ref();
		// 写入0层文件
		Status s = WriteLevel0Table(imm_, &edit, base);
		base->Unref();

		if (s.ok() && shutting_down_.load(std::memory_order_acquire)) {
			s = Status::IOError("Deleting DB during memtable compaction");
		}

		// Replace immutable memtable with the generated Table
		if (s.ok()) {
			edit.SetPrevLogNumber(0);
			edit.SetLogNumber(logfile_number_);  // Earlier logs no longer needed
			// 记录新版本 version
			s = versions_->LogAndApply(&edit, &mutex_);
		}

		if (s.ok()) {
			// Commit to the new state
			imm_->Unref();
			imm_ = nullptr;
			has_imm_.store(false, std::memory_order_release);
			//移出旧的文件
			RemoveObsoleteFiles();
		} else {
			RecordBackgroundError(s);
		}
	}

	void DBImpl::CompactRange(const Slice *begin, const Slice *end) {
		int max_level_with_files = 1;
		{
			MutexLock l(&mutex_);
			Version *base = versions_->current();
			for (int level = 1; level < config::kNumLevels; level++) {
				if (base->OverlapInLevel(level, begin, end)) {
					max_level_with_files = level;
				}
			}
		}
		TEST_CompactMemTable();  // TODO(sanjay): Skip if memtable does not overlap
		for (int level = 0; level < max_level_with_files; level++) {
			TEST_CompactRange(level, begin, end);
		}
	}

	void DBImpl::TEST_CompactRange(int level, const Slice *begin,
								   const Slice *end) {
		assert(level >= 0);
		assert(level + 1 < config::kNumLevels);

		InternalKey begin_storage, end_storage;

		ManualCompaction manual;
		manual.level = level;
		manual.done = false;
		if (begin == nullptr) {
			manual.begin = nullptr;
		} else {
			begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
			manual.begin = &begin_storage;
		}
		if (end == nullptr) {
			manual.end = nullptr;
		} else {
			end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
			manual.end = &end_storage;
		}

		MutexLock l(&mutex_);
		while (!manual.done && !shutting_down_.load(std::memory_order_acquire) &&
			   bg_error_.ok()) {
			if (manual_compaction_ == nullptr) {  // Idle
				manual_compaction_ = &manual;
				MaybeScheduleCompaction();
			} else {  // Running either my compaction or another compaction.
				background_work_finished_signal_.Wait();
			}
		}
		if (manual_compaction_ == &manual) {
			// Cancel my manual compaction since we aborted early for some reason.
			manual_compaction_ = nullptr;
		}
	}

	Status DBImpl::TEST_CompactMemTable() {
		// nullptr batch means just wait for earlier writes to be done
		Status s = Write(WriteOptions(), nullptr);
		if (s.ok()) {
			// Wait until the compaction completes
			MutexLock l(&mutex_);
			while (imm_ != nullptr && bg_error_.ok()) {
				background_work_finished_signal_.Wait();
			}
			if (imm_ != nullptr) {
				s = bg_error_;
			}
		}
		return s;
	}

	void DBImpl::RecordBackgroundError(const Status &s) {
		mutex_.AssertHeld();
		if (bg_error_.ok()) {
			bg_error_ = s;
			background_work_finished_signal_.SignalAll();
		}
	}

	// 判断是否 要 调用去 compact
	void DBImpl::MaybeScheduleCompaction() {
		mutex_.AssertHeld();
		if (background_compaction_scheduled_) {
			// Already scheduled
		} else if (shutting_down_.load(std::memory_order_acquire)) {
			// DB is being deleted; no more background compactions
		} else if (!bg_error_.ok()) {
			// Already got an error; no more changes
		} else if (imm_ == nullptr && manual_compaction_ == nullptr && !versions_->NeedsCompaction()) {
			// No work to be done
			// manual_compaction_ != nullptr 表示是 调用 CompactRange 触发
			// 这里 不变导致无限递归
		} else {
			background_compaction_scheduled_ = true;
			env_->Schedule(&DBImpl::BGWork, this);
		}
	}

	void DBImpl::BGWork(void *db) {
		reinterpret_cast<DBImpl *>(db)->BackgroundCall();
	}

// 后台任务
	void DBImpl::BackgroundCall() {
		MutexLock l(&mutex_);
		assert(background_compaction_scheduled_);
		if (shutting_down_.load(std::memory_order_acquire)) {
			// No more background work when shutting down.
		} else if (!bg_error_.ok()) {
			// No more background work after a background error.
		} else {
			BackgroundCompaction();
		}

		background_compaction_scheduled_ = false;  // 后台压缩结束

		// Previous compaction may have produced too many files in a level,
		// so reschedule another compaction if needed.
		MaybeScheduleCompaction(); // 可能生成文件太多, 再开启一轮压缩, 以便进一步下沉
		background_work_finished_signal_.SignalAll();  // 通知所有, 压缩已完成
	}

	void DBImpl::BackgroundCompaction() {
		mutex_.AssertHeld();

		if (imm_ != nullptr) {
			// imm -> l0
			CompactMemTable();
			return;
		}

		Compaction *c;
		bool is_manual = (manual_compaction_ != nullptr);
		InternalKey manual_end;
		if (is_manual) { // 主动压缩, 一些工具需要用到, 定时压缩
			ManualCompaction *m = manual_compaction_;
			c = versions_->CompactRange(m->level, m->begin, m->end);
			m->done = (c == nullptr);
			if (c != nullptr) {
				manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
			}
			Log(options_.info_log,
				"Manual compaction at level-%d from %s .. %s; will stop at %s\n",
				m->level, (m->begin ? m->begin->DebugString().c_str() : "(begin)"),
				(m->end ? m->end->DebugString().c_str() : "(end)"),
				(m->done ? "(end)" : manual_end.DebugString().c_str()));
		} else {
			// 挑选2个不同层级的sst，合并
			c = versions_->PickCompaction();
		}

		Status status;
		if (c == nullptr) {
			// Nothing to do
		} else if (!is_manual && c->IsTrivialMove()) {
			// Move file to next level
			assert(c->num_input_files(0) == 1);
			FileMetaData *f = c->input(0, 0);
			c->edit()->RemoveFile(c->level(), f->number);
			c->edit()->AddFile(c->level() + 1, f->number, f->file_size, f->smallest,
							   f->largest);
			status = versions_->LogAndApply(c->edit(), &mutex_);
			if (!status.ok()) {
				RecordBackgroundError(status);
			}
			VersionSet::LevelSummaryStorage tmp;
			Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
				static_cast<unsigned long long>(f->number), c->level() + 1,
				static_cast<unsigned long long>(f->file_size),
				status.ToString().c_str(), versions_->LevelSummary(&tmp));
		} else {
			CompactionState *compact = new CompactionState(c);
			//进行合并
			status = DoCompactionWork(compact);
			if (!status.ok()) {
				RecordBackgroundError(status);
			}
			CleanupCompaction(compact);
			c->ReleaseInputs();
			//删除就文件
			RemoveObsoleteFiles();
		}
		delete c;

		if (status.ok()) {
			// Done
		} else if (shutting_down_.load(std::memory_order_acquire)) {
			// Ignore compaction errors found during shutting down
		} else {
			Log(options_.info_log, "Compaction error: %s", status.ToString().c_str());
		}

		if (is_manual) {
			ManualCompaction *m = manual_compaction_;
			if (!status.ok()) {
				m->done = true;
			}
			if (!m->done) {
				// We only compacted part of the requested range.  Update *m
				// to the range that is left to be compacted.
				m->tmp_storage = manual_end;
				m->begin = &m->tmp_storage;
			}
			manual_compaction_ = nullptr;
		}
	}

	void DBImpl::CleanupCompaction(CompactionState *compact) {
		mutex_.AssertHeld();
		if (compact->builder != nullptr) {
			// May happen if we get a shutdown call in the middle of compaction
			compact->builder->Abandon();
			delete compact->builder;
		} else {
			assert(compact->outfile == nullptr);
		}
		delete compact->outfile;
		for (size_t i = 0; i < compact->outputs.size(); i++) {
			const CompactionState::Output &out = compact->outputs[i];
			pending_outputs_.erase(out.number);
		}
		delete compact;
	}

	Status DBImpl::OpenCompactionOutputFile(CompactionState *compact) {
		assert(compact != nullptr);
		assert(compact->builder == nullptr);
		uint64_t file_number;
		{
			mutex_.Lock();
			file_number = versions_->NewFileNumber();
			pending_outputs_.insert(file_number);
			CompactionState::Output out;
			out.number = file_number;
			out.smallest.Clear();
			out.largest.Clear();
			compact->outputs.push_back(out);
			mutex_.Unlock();
		}

		// Make the output file
		std::string fname = TableFileName(dbname_, file_number);
		Status s = env_->NewWritableFile(fname, &compact->outfile);
		if (s.ok()) {
			compact->builder = new TableBuilder(options_, compact->outfile);
		}
		return s;
	}

	Status DBImpl::FinishCompactionOutputFile(CompactionState *compact,
											  Iterator *input) {
		assert(compact != nullptr);
		assert(compact->outfile != nullptr);
		assert(compact->builder != nullptr);

		const uint64_t output_number = compact->current_output()->number;
		assert(output_number != 0);

		// Check for iterator errors
		Status s = input->status();
		const uint64_t current_entries = compact->builder->NumEntries();
		if (s.ok()) {
			s = compact->builder->Finish();
		} else {
			compact->builder->Abandon();
		}
		const uint64_t current_bytes = compact->builder->FileSize();
		compact->current_output()->file_size = current_bytes;
		compact->total_bytes += current_bytes;
		delete compact->builder;
		compact->builder = nullptr;

		// Finish and check for file errors
		if (s.ok()) {
			s = compact->outfile->Sync();
		}
		if (s.ok()) {
			s = compact->outfile->Close();
		}
		delete compact->outfile;
		compact->outfile = nullptr;

		if (s.ok() && current_entries > 0) {
			// Verify that the table is usable
			Iterator *iter =
					table_cache_->NewIterator(ReadOptions(), output_number, current_bytes);
			s = iter->status();
			delete iter;
			if (s.ok()) {
				Log(options_.info_log, "Generated table #%llu@%d: %lld keys, %lld bytes",
					(unsigned long long) output_number, compact->compaction->level(),
					(unsigned long long) current_entries,
					(unsigned long long) current_bytes);
			}
		}
		return s;
	}

	Status DBImpl::InstallCompactionResults(CompactionState *compact) {
		mutex_.AssertHeld();
		Log(options_.info_log, "Compacted %d@%d + %d@%d files => %lld bytes",
			compact->compaction->num_input_files(0), compact->compaction->level(),
			compact->compaction->num_input_files(1), compact->compaction->level() + 1,
			static_cast<long long>(compact->total_bytes));

		// Add compaction outputs
		compact->compaction->AddInputDeletions(compact->compaction->edit());
		const int level = compact->compaction->level();
		for (size_t i = 0; i < compact->outputs.size(); i++) {
			const CompactionState::Output &out = compact->outputs[i];
			compact->compaction->edit()->AddFile(level + 1, out.number, out.file_size,
												 out.smallest, out.largest);
		}
		return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
	}

//后台压缩 两个sst  归并排序
	Status DBImpl::DoCompactionWork(CompactionState *compact) {
		const uint64_t start_micros = env_->NowMicros();
		int64_t imm_micros = 0;  // Micros spent doing imm_ compactions

		Log(options_.info_log, "Compacting %d@%d + %d@%d files",
			compact->compaction->num_input_files(0), compact->compaction->level(),
			compact->compaction->num_input_files(1),
			compact->compaction->level() + 1);

		assert(versions_->NumLevelFiles(compact->compaction->level()) > 0);
		assert(compact->builder == nullptr);
		assert(compact->outfile == nullptr);
		if (snapshots_.empty()) {
			compact->smallest_snapshot = versions_->LastSequence();
		} else {
			compact->smallest_snapshot = snapshots_.oldest()->sequence_number();
		}

		Iterator *input = versions_->MakeInputIterator(compact->compaction);

		// Release mutex while we're actually doing the compaction work
		mutex_.Unlock();

		input->SeekToFirst();
		Status status;
		ParsedInternalKey ikey;
		std::string current_user_key;
		bool has_current_user_key = false;
		SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
		while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {
			// Prioritize immutable compaction work
			if (has_imm_.load(std::memory_order_relaxed)) {
				const uint64_t imm_start = env_->NowMicros();
				mutex_.Lock();
				if (imm_ != nullptr) {
					CompactMemTable();
					// Wake up MakeRoomForWrite() if necessary.
					background_work_finished_signal_.SignalAll();
				}
				mutex_.Unlock();
				imm_micros += (env_->NowMicros() - imm_start);
			}

			Slice key = input->key();
			if (compact->compaction->ShouldStopBefore(key) &&
				compact->builder != nullptr) {
				status = FinishCompactionOutputFile(compact, input);
				if (!status.ok()) {
					break;
				}
			}

			// Handle key/value, add to state, etc.
			bool drop = false;
			if (!ParseInternalKey(key, &ikey)) {
				// Do not hide error keys
				current_user_key.clear();
				has_current_user_key = false;
				last_sequence_for_key = kMaxSequenceNumber;
			} else {
				if (!has_current_user_key ||
					user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) !=
					0) {
					// First occurrence of this user key
					current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
					has_current_user_key = true;
					last_sequence_for_key = kMaxSequenceNumber;
				}

				if (last_sequence_for_key <= compact->smallest_snapshot) {
					// Hidden by an newer entry for same user key
					drop = true;  // (A)
				} else if (ikey.type == kTypeDeletion &&
						   ikey.sequence <= compact->smallest_snapshot &&
						   compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
					// For this user key:
					// (1) there is no data in higher levels
					// (2) data in lower levels will have larger sequence numbers
					// (3) data in layers that are being compacted here and have
					//     smaller sequence numbers will be dropped in the next
					//     few iterations of this loop (by rule (A) above).
					// Therefore this deletion marker is obsolete and can be dropped.
					drop = true;
				}

				last_sequence_for_key = ikey.sequence;
			}
#if 0
			Log(options_.info_log,
				"  Compact: %s, seq %d, type: %d %d, drop: %d, is_base: %d, "
				"%d smallest_snapshot: %d",
				ikey.user_key.ToString().c_str(),
				(int)ikey.sequence, ikey.type, kTypeValue, drop,
				compact->compaction->IsBaseLevelForKey(ikey.user_key),
				(int)last_sequence_for_key, (int)compact->smallest_snapshot);
#endif

			if (!drop) {
				// Open output file if necessary
				if (compact->builder == nullptr) {
					status = OpenCompactionOutputFile(compact);
					if (!status.ok()) {
						break;
					}
				}
				if (compact->builder->NumEntries() == 0) {
					compact->current_output()->smallest.DecodeFrom(key);
				}
				compact->current_output()->largest.DecodeFrom(key);
				compact->builder->Add(key, input->value());

				// Close output file if it is big enough
				if (compact->builder->FileSize() >=
					compact->compaction->MaxOutputFileSize()) {
					status = FinishCompactionOutputFile(compact, input);
					if (!status.ok()) {
						break;
					}
				}
			}

			input->Next();
		}

		if (status.ok() && shutting_down_.load(std::memory_order_acquire)) {
			status = Status::IOError("Deleting DB during compaction");
		}
		if (status.ok() && compact->builder != nullptr) {
			status = FinishCompactionOutputFile(compact, input);
		}
		if (status.ok()) {
			status = input->status();
		}
		delete input;
		input = nullptr;

		CompactionStats stats;
		stats.micros = env_->NowMicros() - start_micros - imm_micros;
		for (int which = 0; which < 2; which++) {
			for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
				stats.bytes_read += compact->compaction->input(which, i)->file_size;
			}
		}
		for (size_t i = 0; i < compact->outputs.size(); i++) {
			stats.bytes_written += compact->outputs[i].file_size;
		}

		mutex_.Lock();
		stats_[compact->compaction->level() + 1].Add(stats);

		if (status.ok()) {
			status = InstallCompactionResults(compact);
		}
		if (!status.ok()) {
			RecordBackgroundError(status);
		}
		VersionSet::LevelSummaryStorage tmp;
		Log(options_.info_log, "compacted to: %s", versions_->LevelSummary(&tmp));
		return status;
	}

	namespace {

		struct IterState {
			port::Mutex *const mu;
			Version *const version GUARDED_BY(mu);
			MemTable *const mem GUARDED_BY(mu);
			MemTable *const imm GUARDED_BY(mu);

			IterState(port::Mutex *mutex, MemTable *mem, MemTable *imm, Version *version)
					: mu(mutex), version(version), mem(mem), imm(imm) {}
		};

		static void CleanupIteratorState(void *arg1, void *arg2) {
			IterState *state = reinterpret_cast<IterState *>(arg1);
			state->mu->Lock();
			state->mem->Unref();
			if (state->imm != nullptr) state->imm->Unref();
			state->version->Unref();
			state->mu->Unlock();
			delete state;
		}

	}  // anonymous namespace

	Iterator *DBImpl::NewInternalIterator(const ReadOptions &options,
										  SequenceNumber *latest_snapshot,
										  uint32_t *seed) {
		mutex_.Lock();
		*latest_snapshot = versions_->LastSequence();

		// Collect together all needed child iterators
		std::vector<Iterator *> list;
		list.push_back(mem_->NewIterator());
		mem_->Ref();
		if (imm_ != nullptr) {
			list.push_back(imm_->NewIterator());
			imm_->Ref();
		}
		versions_->current()->AddIterators(options, &list);
		Iterator *internal_iter =
				NewMergingIterator(&internal_comparator_, &list[0], list.size());
		versions_->current()->Ref();

		IterState *cleanup = new IterState(&mutex_, mem_, imm_, versions_->current());
		internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, nullptr);

		*seed = ++seed_;
		mutex_.Unlock();
		return internal_iter;
	}

	Iterator *DBImpl::TEST_NewInternalIterator() {
		SequenceNumber ignored;
		uint32_t ignored_seed;
		return NewInternalIterator(ReadOptions(), &ignored, &ignored_seed);
	}

	int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
		MutexLock l(&mutex_);
		return versions_->MaxNextLevelOverlappingBytes();
	}

	// 读取数据
	Status DBImpl::Get(const ReadOptions &options, const Slice &key, std::string *value) {
		Status s;
		MutexLock l(&mutex_);  // 加锁 raii 资源获取 is 初始化

		//计算版本号
		SequenceNumber snapshot;
		if (options.snapshot != nullptr) {
			snapshot = static_cast<const SnapshotImpl *>(options.snapshot)->sequence_number();
		} else {
			snapshot = versions_->LastSequence();
		}

		MemTable *mem = mem_;
		MemTable *imm = imm_;
		// 当前 数据文件的 版本
		Version *current = versions_->current();
		//引用计数，
		mem->Ref();
		if (imm != nullptr) imm->Ref();
		current->Ref();

		bool have_stat_update = false;
		Version::GetStats stats;

		// Unlock while reading from files and memtables
		{
			mutex_.Unlock();
			// First look in the memtable, then in the immutable memtable (if any).
			LookupKey lkey(key, snapshot); //构造查询key对象
			// 先到内存表
			if (mem->Get(lkey, value, &s)) {
				// Done
			} else if (imm != nullptr && imm->Get(lkey, value, &s)) {
				//可变表找不到再到不变内存表
				// Done
			} else {
				//从数据文件里找
				s = current->Get(options, lkey, value, &stats);
				have_stat_update = true;
			}
			mutex_.Lock();
		}

		if (have_stat_update && current->UpdateStats(stats)) {
			//判断是否进行压缩
			MaybeScheduleCompaction();
		}
		//减引用
		mem->Unref();
		if (imm != nullptr) imm->Unref();
		current->Unref();
		return s;
	}

	Iterator *DBImpl::NewIterator(const ReadOptions &options) {
		SequenceNumber latest_snapshot;
		uint32_t seed;
		Iterator *iter = NewInternalIterator(options, &latest_snapshot, &seed);
		return NewDBIterator(this, user_comparator(), iter,
							 (options.snapshot != nullptr
							  ? static_cast<const SnapshotImpl *>(options.snapshot)
									  ->sequence_number()
							  : latest_snapshot),
							 seed);
	}

	void DBImpl::RecordReadSample(Slice key) {
		MutexLock l(&mutex_);
		if (versions_->current()->RecordReadSample(key)) {
			MaybeScheduleCompaction();
		}
	}

	const Snapshot *DBImpl::GetSnapshot() {
		MutexLock l(&mutex_);
		return snapshots_.New(versions_->LastSequence());
	}

	void DBImpl::ReleaseSnapshot(const Snapshot *snapshot) {
		MutexLock l(&mutex_);
		snapshots_.Delete(static_cast<const SnapshotImpl *>(snapshot));
	}

	// 方便函数 Convenience methods
	Status DBImpl::Put(const WriteOptions &o, const Slice &key, const Slice &val) {
		return DB::Put(o, key, val);
	}

	Status DBImpl::Delete(const WriteOptions &options, const Slice &key) {
		return DB::Delete(options, key);
	}

	// put 和 delete 最终也是write方法
	Status DBImpl::Write(const WriteOptions &options, WriteBatch *updates) {
		Writer w(&mutex_);
		w.batch = updates;
		w.sync = options.sync;  //根据配置决定是否立即同步到磁盘
		w.done = false;

		MutexLock l(&mutex_);  //构造函数，会加锁，析构函数会解锁
		writers_.push_back(&w);   // 入队列
		// 控制并发 阻塞指定写完成，或者是头一个写任务
		while (!w.done && &w != writers_.front()) {
			w.cv.Wait();
		}

		// 第一个 或者 已经被其他线程done 都会过来
		if (w.done) {  //排除掉 已经写完成的情况
			return w.status;
		}

		// May temporarily unlock and wait.
		// 1 新建内存表，整理内存磁盘准备空间
		Status status = MakeRoomForWrite(updates == nullptr);
		uint64_t last_sequence = versions_->LastSequence();   //快照版本号
		Writer *last_writer = &w;

		if (status.ok() && updates != nullptr) {  // nullptr batch is for compactions
			WriteBatch *write_batch = BuildBatchGroup(&last_writer);
			// 插入序号
			WriteBatchInternal::SetSequence(write_batch, last_sequence + 1);
			// 更新序号， 加上条目数
			last_sequence += WriteBatchInternal::Count(write_batch);

			// Add to log and apply to memtable. 写入只需要日志和写入跳表两步
			// We can release the lock during this phase since &w(Writer) is currently responsible for logging
			// and protects against concurrent loggers and concurrent writes into mem_. 解锁后怎么保护的并发？
			{
				mutex_.Unlock();
				// 2 追加日志 将writeBatch 字符串整个 写到日志
				status = log_->AddRecord(WriteBatchInternal::Contents(write_batch));
				bool sync_error = false;
				if (status.ok() && options.sync) {
					// 开启sync, 只是对日志文件的同步磁盘生效
					status = logfile_->Sync();
					if (!status.ok()) {
						sync_error = true;
					}
				}
				if (status.ok()) {
					// 3. 插入到 memtable，其中每写入一个kv sequence都+1
					status = WriteBatchInternal::InsertInto(write_batch, mem_);
				}
				mutex_.Lock();  //上面析构函数解锁
				if (sync_error) {
					// The state of the log file is indeterminate不确定状态，尚未持久化:
					// the log record we just added may or may not show up when the DB is re-opened. 不确定重启的时候是前进还是后退
					// So we force the DB into a mode where all future writes fail 让之后的写入都失败.  //为什么这样做
					RecordBackgroundError(status);
				}
			}
			// tmp_batch_ 是啥
			if (write_batch == tmp_batch_) tmp_batch_->Clear();

			//更新最终的序号，版本号，
			versions_->SetLastSequence(last_sequence);
		}

		while (true) {
			// 出队列
			Writer *ready = writers_.front();
			writers_.pop_front();

			if (ready != &w) {
				ready->status = status; //保存处理状态
				ready->done = true; //标记写入完成
				ready->cv.Signal(); //唤醒当前线程
			}
			// ready == &w 不处理？
			if (ready == last_writer) break;
		}

		// Notify new head of write queue
		if (!writers_.empty()) {
			//之前的都写完了，唤醒之后的头部，继续处理
			writers_.front()->cv.Signal();
		}

		return status;
	}

// REQUIRES: Writer list must be non-empty
// REQUIRES: First writer must have a non-null batch
	WriteBatch *DBImpl::BuildBatchGroup(Writer **last_writer) {
		mutex_.AssertHeld();
		assert(!writers_.empty());

		Writer *first = writers_.front();
		WriteBatch *result = first->batch;
		assert(result != nullptr);  //第一个writer的批量写任务，必须要有

		size_t size = WriteBatchInternal::ByteSize(first->batch);

		// Allow the group to grow up to a maximum size, but if the
		// original write is small, limit the growth so we do not slow
		// down the small write too much.
		size_t max_size = 1 << 20; //1M
		if (size <= (128 << 10)) { // 128K
			max_size = size + (128 << 10);
		}

		*last_writer = first;
		std::deque<Writer *>::iterator iter = writers_.begin();
		++iter;  // 越过 第一个 Advance past "first"
		for (; iter != writers_.end(); ++iter) {
			Writer *w = *iter;
			if (w->sync && !first->sync) {
				// Do not include a sync write into a batch handled by a non-sync write.
				break;
			}

			if (w->batch != nullptr) {
				// 长度累加
				size += WriteBatchInternal::ByteSize(w->batch);
				if (size > max_size) {
					// Do not make batch too big
					break;
				}

				// Append to *result
				// first 分开处理
				if (result == first->batch) {
					// Switch to temporary batch instead of disturbing caller's batch
					result = tmp_batch_;
					assert(WriteBatchInternal::Count(result) == 0);
					WriteBatchInternal::Append(result, first->batch);
				}
				WriteBatchInternal::Append(result, w->batch);
				//所有操作，都打包都一个
			}
			// 记录合并后的尾部writer
			*last_writer = w;
		}
		return result;
	}

// REQUIRES: mutex_ is held
// REQUIRES: this thread is currently at the front of the writer queue
	Status DBImpl::MakeRoomForWrite(bool force) {  //force代表是否强制一次compact
		mutex_.AssertHeld();
		assert(!writers_.empty());

		bool allow_delay = !force;  //基本 force 都是true
		Status s;
		while (true) {
			if (!bg_error_.ok()) {
				// Yield拿出 previous error， 让之后的写都失败
				s = bg_error_;
				break;
			} else if (allow_delay && versions_->NumLevelFiles(0) >= config::kL0_SlowdownWritesTrigger) {
				// 允许延迟
				// We are getting close to hitting a hard limit on the number of
				// L0 files.  Rather than delaying a single write by several
				// seconds when we hit the hard limit, start delaying each
				// individual write by 1ms to reduce latency variance.  Also,
				// this delay hands over some CPU to the compaction thread in
				// case it is sharing the same core as the writer.
				mutex_.Unlock();
				//当前线程，睡眠一段时间，减慢写入
				env_->SleepForMicroseconds(1000);
				allow_delay = false;  // 只延迟一次 Do not delay a single write more than once
				mutex_.Lock();
			} else if (!force && (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
				// !force 标识不用强制compact
				// There is room in current memtable
				break;  //有空间了，跳出循环，继续之后的写入
			} else if (imm_ != nullptr) {  //这里可变表已经写满
				// We have filled up the current memtable, but the previous
				// one is still being compacted, so we wait. 当前的已经填满，
				// 之前的不变表在压缩，memtable不能变为不变表，再新建memtable，只能等待
				Log(options_.info_log, "Current memtable full; waiting...\n");
				background_work_finished_signal_.Wait();
			} else if (versions_->NumLevelFiles(0) >= config::kL0_StopWritesTrigger) {
				// There are too many level-0 files.
				// //停止写入，等待0层文件合并压缩完成，减少数量
				Log(options_.info_log, "Too many L0 files; waiting...\n");
				background_work_finished_signal_.Wait();
			} else {
				// 切换为不变表， 新建内存表，并开启压缩不变表
				// Attempt to switch to a new memtable and trigger compaction of old
				assert(versions_->PrevLogNumber() == 0);

				uint64_t new_log_number = versions_->NewFileNumber();
				WritableFile *lfile = nullptr;
				// 创建新的日志文件，env代表操作系统环境
				s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
				if (!s.ok()) {
					// Avoid chewing through file number space in a tight loop.
					versions_->ReuseFileNumber(new_log_number);
					break;
				}

				delete log_;
				delete logfile_;
				logfile_ = lfile;  //保存新的日志文件
				logfile_number_ = new_log_number;
				log_ = new log::Writer(lfile); //新的日志器

				imm_ = mem_;
				has_imm_.store(true, std::memory_order_release);
				// 新建 memtable
				mem_ = new MemTable(internal_comparator_);
				mem_->Ref();
				force = false;  // Do not force another compaction if have room
				// 开启 minor compact
				MaybeScheduleCompaction();
			}
		}
		return s;
	}

	bool DBImpl::GetProperty(const Slice &property, std::string *value) {
		value->clear();

		MutexLock l(&mutex_);
		Slice in = property;
		Slice prefix("leveldb.");
		if (!in.starts_with(prefix)) return false;
		in.remove_prefix(prefix.size());

		if (in.starts_with("num-files-at-level")) {
			in.remove_prefix(strlen("num-files-at-level"));
			uint64_t level;
			bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
			if (!ok || level >= config::kNumLevels) {
				return false;
			} else {
				char buf[100];
				std::snprintf(buf, sizeof(buf), "%d",
							  versions_->NumLevelFiles(static_cast<int>(level)));
				*value = buf;
				return true;
			}
		} else if (in == "stats") {
			char buf[200];
			std::snprintf(buf, sizeof(buf),
						  "                               Compactions\n"
						  "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
						  "--------------------------------------------------\n");
			value->append(buf);
			for (int level = 0; level < config::kNumLevels; level++) {
				int files = versions_->NumLevelFiles(level);
				if (stats_[level].micros > 0 || files > 0) {
					std::snprintf(buf, sizeof(buf), "%3d %8d %8.0f %9.0f %8.0f %9.0f\n",
								  level, files, versions_->NumLevelBytes(level) / 1048576.0,
								  stats_[level].micros / 1e6,
								  stats_[level].bytes_read / 1048576.0,
								  stats_[level].bytes_written / 1048576.0);
					value->append(buf);
				}
			}
			return true;
		} else if (in == "sstables") {
			*value = versions_->current()->DebugString();
			return true;
		} else if (in == "approximate-memory-usage") {
			size_t total_usage = options_.block_cache->TotalCharge();
			if (mem_) {
				total_usage += mem_->ApproximateMemoryUsage();
			}
			if (imm_) {
				total_usage += imm_->ApproximateMemoryUsage();
			}
			char buf[50];
			std::snprintf(buf, sizeof(buf), "%llu",
						  static_cast<unsigned long long>(total_usage));
			value->append(buf);
			return true;
		}

		return false;
	}

	void DBImpl::GetApproximateSizes(const Range *range, int n, uint64_t *sizes) {
		// TODO(opt): better implementation
		MutexLock l(&mutex_);
		Version *v = versions_->current();
		v->Ref();

		for (int i = 0; i < n; i++) {
			// Convert user_key into a corresponding internal key.
			InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
			InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
			uint64_t start = versions_->ApproximateOffsetOf(v, k1);
			uint64_t limit = versions_->ApproximateOffsetOf(v, k2);
			sizes[i] = (limit >= start ? limit - start : 0);
		}

		v->Unref();
	}

// Default implementations of convenience methods that subclasses of DB
// can call if they wish
	Status DB::Put(const WriteOptions &opt, const Slice &key, const Slice &value) {
		WriteBatch batch;
		batch.Put(key, value);
		return Write(opt, &batch);
	}

	Status DB::Delete(const WriteOptions &opt, const Slice &key) {
		WriteBatch batch;
		batch.Delete(key);
		return Write(opt, &batch);
	}

	DB::~DB() = default;

	Status DB::Open(const Options &options, const std::string &dbname, DB **dbptr) {
		*dbptr = nullptr;

		// 创建 db 实例
		DBImpl *impl = new DBImpl(options, dbname);
		impl->mutex_.Lock();
		VersionEdit edit;
		// Recover handles create_if_missing, error_if_exists
		bool save_manifest = false;
		// 恢复数据, 建立元数据
		Status s = impl->Recover(&edit, &save_manifest);

		if (s.ok() && impl->mem_ == nullptr) {  //新建log文件和 memtable
			// Create new log and a corresponding memtable.
			uint64_t new_log_number = impl->versions_->NewFileNumber();
			WritableFile *lfile;
			s = options.env->NewWritableFile(LogFileName(dbname, new_log_number), &lfile);

			if (s.ok()) {
				edit.SetLogNumber(new_log_number);
				impl->logfile_ = lfile;
				impl->logfile_number_ = new_log_number;
				impl->log_ = new log::Writer(lfile);  // 新的日志器
				//创建新的内存表
				impl->mem_ = new MemTable(impl->internal_comparator_);
				impl->mem_->Ref();
			}
		}

		if (s.ok() && save_manifest) {
			edit.SetPrevLogNumber(0);  // No older logs needed after recovery.
			edit.SetLogNumber(impl->logfile_number_);
			// 应用 修改 保存到 manifest文件
			s = impl->versions_->LogAndApply(&edit, &impl->mutex_);
		}

		if (s.ok()) {
			impl->RemoveObsoleteFiles();  // 删除过期文件
			impl->MaybeScheduleCompaction(); // 开后台的compact
		}

		impl->mutex_.Unlock();
		if (s.ok()) {
			assert(impl->mem_ != nullptr);
			*dbptr = impl;
		} else {
			delete impl;
		}
		return s;
	}

	Snapshot::~Snapshot() = default;

	Status DestroyDB(const std::string &dbname, const Options &options) {
		Env *env = options.env;
		std::vector<std::string> filenames;
		Status result = env->GetChildren(dbname, &filenames);
		if (!result.ok()) {
			// Ignore error in case directory does not exist
			return Status::OK();
		}

		FileLock *lock;
		const std::string lockname = LockFileName(dbname);
		result = env->LockFile(lockname, &lock);
		if (result.ok()) {
			uint64_t number;
			FileType type;
			for (size_t i = 0; i < filenames.size(); i++) {
				if (ParseFileName(filenames[i], &number, &type) &&
					type != kDBLockFile) {  // Lock file will be deleted at end
					Status del = env->RemoveFile(dbname + "/" + filenames[i]);
					if (result.ok() && !del.ok()) {
						result = del;
					}
				}
			}
			env->UnlockFile(lock);  // Ignore error since state is already gone
			env->RemoveFile(lockname);
			env->RemoveDir(dbname);  // Ignore error in case dir contains other files
		}
		return result;
	}

}  // namespace leveldb
