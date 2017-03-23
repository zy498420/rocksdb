//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#if !defined(ROCKSDB_LITE) && !defined(OS_WIN)

#include <algorithm>
#include <string>

#include "db/db_impl.h"
#include "db/filename.h"
#include "port/port.h"
#include "port/stack_trace.h"
#include "rocksdb/rate_limiter.h"
#include "rocksdb/transaction_log.h"
#include "rocksdb/types.h"
#include "rocksdb/utilities/backupable_db.h"
#include "rocksdb/utilities/options_util.h"
#include "util/env_chroot.h"
#include "util/file_reader_writer.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/stderr_logger.h"
#include "util/string_util.h"
#include "util/sync_point.h"
#include "util/testharness.h"
#include "util/testutil.h"

namespace rocksdb {

namespace {

using std::unique_ptr;

class DummyDB : public StackableDB {
 public:
  /* implicit */
  DummyDB(const Options& options, const std::string& dbname)
     : StackableDB(nullptr), options_(options), dbname_(dbname),
       deletions_enabled_(true), sequence_number_(0) {}

  virtual SequenceNumber GetLatestSequenceNumber() const override {
    return ++sequence_number_;
  }

  virtual const std::string& GetName() const override {
    return dbname_;
  }

  virtual Env* GetEnv() const override {
    return options_.env;
  }

  using DB::GetOptions;
  virtual Options GetOptions(ColumnFamilyHandle* column_family) const override {
    return options_;
  }

  virtual Status EnableFileDeletions(bool force) override {
    EXPECT_TRUE(!deletions_enabled_);
    deletions_enabled_ = true;
    return Status::OK();
  }

  virtual Status DisableFileDeletions() override {
    EXPECT_TRUE(deletions_enabled_);
    deletions_enabled_ = false;
    return Status::OK();
  }

  virtual Status GetLiveFiles(std::vector<std::string>& vec, uint64_t* mfs,
                              bool flush_memtable = true) override {
    EXPECT_TRUE(!deletions_enabled_);
    vec = live_files_;
    *mfs = 100;
    return Status::OK();
  }

  virtual ColumnFamilyHandle* DefaultColumnFamily() const override {
    return nullptr;
  }

  class DummyLogFile : public LogFile {
   public:
    /* implicit */
     DummyLogFile(const std::string& path, bool alive = true)
         : path_(path), alive_(alive) {}

    virtual std::string PathName() const override {
      return path_;
    }

    virtual uint64_t LogNumber() const override {
      // what business do you have calling this method?
      EXPECT_TRUE(false);
      return 0;
    }

    virtual WalFileType Type() const override {
      return alive_ ? kAliveLogFile : kArchivedLogFile;
    }

    virtual SequenceNumber StartSequence() const override {
      // backupabledb should not need this method
      EXPECT_TRUE(false);
      return 0;
    }

    virtual uint64_t SizeFileBytes() const override {
      return 0;
    }

   private:
    std::string path_;
    bool alive_;
  }; // DummyLogFile

  virtual Status GetSortedWalFiles(VectorLogPtr& files) override {
    EXPECT_TRUE(!deletions_enabled_);
    files.resize(wal_files_.size());
    for (size_t i = 0; i < files.size(); ++i) {
      files[i].reset(
          new DummyLogFile(wal_files_[i].first, wal_files_[i].second));
    }
    return Status::OK();
  }

  std::vector<std::string> live_files_;
  // pair<filename, alive?>
  std::vector<std::pair<std::string, bool>> wal_files_;
 private:
  Options options_;
  std::string dbname_;
  bool deletions_enabled_;
  mutable SequenceNumber sequence_number_;
}; // DummyDB

class TestEnv : public EnvWrapper {
 public:
  explicit TestEnv(Env* t) : EnvWrapper(t) {}

  class DummySequentialFile : public SequentialFile {
   public:
    explicit DummySequentialFile(bool fail_reads)
        : SequentialFile(), rnd_(5), fail_reads_(fail_reads) {}
    virtual Status Read(size_t n, Slice* result, char* scratch) override {
      if (fail_reads_) {
        return Status::IOError();
      }
      size_t read_size = (n > size_left) ? size_left : n;
      for (size_t i = 0; i < read_size; ++i) {
        scratch[i] = rnd_.Next() & 255;
      }
      *result = Slice(scratch, read_size);
      size_left -= read_size;
      return Status::OK();
    }

    virtual Status Skip(uint64_t n) override {
      size_left = (n > size_left) ? size_left - n : 0;
      return Status::OK();
    }
   private:
    size_t size_left = 200;
    Random rnd_;
    bool fail_reads_;
  };

  Status NewSequentialFile(const std::string& f, unique_ptr<SequentialFile>* r,
                           const EnvOptions& options) override {
    MutexLock l(&mutex_);
    if (dummy_sequential_file_) {
      r->reset(
          new TestEnv::DummySequentialFile(dummy_sequential_file_fail_reads_));
      return Status::OK();
    } else {
      return EnvWrapper::NewSequentialFile(f, r, options);
    }
  }

  Status NewWritableFile(const std::string& f, unique_ptr<WritableFile>* r,
                         const EnvOptions& options) override {
    MutexLock l(&mutex_);
    written_files_.push_back(f);
    if (limit_written_files_ <= 0) {
      return Status::NotSupported("Sorry, can't do this");
    }
    limit_written_files_--;
    return EnvWrapper::NewWritableFile(f, r, options);
  }

  virtual Status DeleteFile(const std::string& fname) override {
    MutexLock l(&mutex_);
    if (fail_delete_files_) {
      return Status::IOError();
    }
    EXPECT_GT(limit_delete_files_, 0U);
    limit_delete_files_--;
    return EnvWrapper::DeleteFile(fname);
  }

  void AssertWrittenFiles(std::vector<std::string>& should_have_written) {
    MutexLock l(&mutex_);
    std::sort(should_have_written.begin(), should_have_written.end());
    std::sort(written_files_.begin(), written_files_.end());

    ASSERT_EQ(should_have_written, written_files_);
  }

  void ClearWrittenFiles() {
    MutexLock l(&mutex_);
    written_files_.clear();
  }

  void SetLimitWrittenFiles(uint64_t limit) {
    MutexLock l(&mutex_);
    limit_written_files_ = limit;
  }

  void SetLimitDeleteFiles(uint64_t limit) {
    MutexLock l(&mutex_);
    limit_delete_files_ = limit;
  }

  void SetDeleteFileFailure(bool fail) {
    MutexLock l(&mutex_);
    fail_delete_files_ = fail;
  }

  void SetDummySequentialFile(bool dummy_sequential_file) {
    MutexLock l(&mutex_);
    dummy_sequential_file_ = dummy_sequential_file;
  }
  void SetDummySequentialFileFailReads(bool dummy_sequential_file_fail_reads) {
    MutexLock l(&mutex_);
    dummy_sequential_file_fail_reads_ = dummy_sequential_file_fail_reads;
  }

  void SetGetChildrenFailure(bool fail) { get_children_failure_ = fail; }
  Status GetChildren(const std::string& dir,
                     std::vector<std::string>* r) override {
    if (get_children_failure_) {
      return Status::IOError("SimulatedFailure");
    }
    return EnvWrapper::GetChildren(dir, r);
  }

  // Some test cases do not actually create the test files (e.g., see
  // DummyDB::live_files_) - for those cases, we mock those files' attributes
  // so CreateNewBackup() can get their attributes.
  void SetFilenamesForMockedAttrs(const std::vector<std::string>& filenames) {
    filenames_for_mocked_attrs_ = filenames;
  }
  Status GetChildrenFileAttributes(
      const std::string& dir, std::vector<Env::FileAttributes>* r) override {
    if (filenames_for_mocked_attrs_.size() > 0) {
      for (const auto& filename : filenames_for_mocked_attrs_) {
        r->push_back({dir + filename, 10 /* size_bytes */});
      }
      return Status::OK();
    }
    return EnvWrapper::GetChildrenFileAttributes(dir, r);
  }

  void SetCreateDirIfMissingFailure(bool fail) {
    create_dir_if_missing_failure_ = fail;
  }
  Status CreateDirIfMissing(const std::string& d) override {
    if (create_dir_if_missing_failure_) {
      return Status::IOError("SimulatedFailure");
    }
    return EnvWrapper::CreateDirIfMissing(d);
  }

  void SetNewDirectoryFailure(bool fail) { new_directory_failure_ = fail; }
  virtual Status NewDirectory(const std::string& name,
                              unique_ptr<Directory>* result) override {
    if (new_directory_failure_) {
      return Status::IOError("SimulatedFailure");
    }
    return EnvWrapper::NewDirectory(name, result);
  }

 private:
  port::Mutex mutex_;
  bool dummy_sequential_file_ = false;
  bool dummy_sequential_file_fail_reads_ = false;
  std::vector<std::string> written_files_;
  std::vector<std::string> filenames_for_mocked_attrs_;
  uint64_t limit_written_files_ = 1000000;
  uint64_t limit_delete_files_ = 1000000;
  bool fail_delete_files_ = false;

  bool get_children_failure_ = false;
  bool create_dir_if_missing_failure_ = false;
  bool new_directory_failure_ = false;
};  // TestEnv

class FileManager : public EnvWrapper {
 public:
  explicit FileManager(Env* t) : EnvWrapper(t), rnd_(5) {}

  Status DeleteRandomFileInDir(const std::string& dir) {
    std::vector<std::string> children;
    GetChildren(dir, &children);
    if (children.size() <= 2) { // . and ..
      return Status::NotFound("");
    }
    while (true) {
      int i = rnd_.Next() % children.size();
      if (children[i] != "." && children[i] != "..") {
        return DeleteFile(dir + "/" + children[i]);
      }
    }
    // should never get here
    assert(false);
    return Status::NotFound("");
  }

  Status AppendToRandomFileInDir(const std::string& dir,
                                 const std::string& data) {
    std::vector<std::string> children;
    GetChildren(dir, &children);
    if (children.size() <= 2) {
      return Status::NotFound("");
    }
    while (true) {
      int i = rnd_.Next() % children.size();
      if (children[i] != "." && children[i] != "..") {
        return WriteToFile(dir + "/" + children[i], data);
      }
    }
    // should never get here
    assert(false);
    return Status::NotFound("");
  }

  Status CorruptFile(const std::string& fname, uint64_t bytes_to_corrupt) {
    std::string file_contents;
    Status s = ReadFileToString(this, fname, &file_contents);
    if (!s.ok()) {
      return s;
    }
    s = DeleteFile(fname);
    if (!s.ok()) {
      return s;
    }

    for (uint64_t i = 0; i < bytes_to_corrupt; ++i) {
      std::string tmp;
      test::RandomString(&rnd_, 1, &tmp);
      file_contents[rnd_.Next() % file_contents.size()] = tmp[0];
    }
    return WriteToFile(fname, file_contents);
  }

  Status CorruptChecksum(const std::string& fname, bool appear_valid) {
    std::string metadata;
    Status s = ReadFileToString(this, fname, &metadata);
    if (!s.ok()) {
      return s;
    }
    s = DeleteFile(fname);
    if (!s.ok()) {
      return s;
    }

    auto pos = metadata.find("private");
    if (pos == std::string::npos) {
      return Status::Corruption("private file is expected");
    }
    pos = metadata.find(" crc32 ", pos + 6);
    if (pos == std::string::npos) {
      return Status::Corruption("checksum not found");
    }

    if (metadata.size() < pos + 7) {
      return Status::Corruption("bad CRC32 checksum value");
    }

    if (appear_valid) {
      if (metadata[pos + 8] == '\n') {
        // single digit value, safe to insert one more digit
        metadata.insert(pos + 8, 1, '0');
      } else {
        metadata.erase(pos + 8, 1);
      }
    } else {
      metadata[pos + 7] = 'a';
    }

    return WriteToFile(fname, metadata);
  }

  Status WriteToFile(const std::string& fname, const std::string& data) {
    unique_ptr<WritableFile> file;
    EnvOptions env_options;
    env_options.use_mmap_writes = false;
    Status s = EnvWrapper::NewWritableFile(fname, &file, env_options);
    if (!s.ok()) {
      return s;
    }
    return file->Append(Slice(data));
  }

 private:
  Random rnd_;
}; // FileManager

// utility functions
static size_t FillDB(DB* db, int from, int to) {
  size_t bytes_written = 0;
  for (int i = from; i < to; ++i) {
    std::string key = "testkey" + ToString(i);
    std::string value = "testvalue" + ToString(i);
    bytes_written += key.size() + value.size();

    EXPECT_OK(db->Put(WriteOptions(), Slice(key), Slice(value)));
  }
  return bytes_written;
}

static void AssertExists(DB* db, int from, int to) {
  for (int i = from; i < to; ++i) {
    std::string key = "testkey" + ToString(i);
    std::string value;
    Status s = db->Get(ReadOptions(), Slice(key), &value);
    ASSERT_EQ(value, "testvalue" + ToString(i));
  }
}

static void AssertEmpty(DB* db, int from, int to) {
  for (int i = from; i < to; ++i) {
    std::string key = "testkey" + ToString(i);
    std::string value = "testvalue" + ToString(i);

    Status s = db->Get(ReadOptions(), Slice(key), &value);
    ASSERT_TRUE(s.IsNotFound());
  }
}

class BackupableDBTest : public testing::Test {
 public:
  BackupableDBTest() {
    // set up files
    std::string db_chroot = test::TmpDir() + "/backupable_db";
    std::string backup_chroot = test::TmpDir() + "/backupable_db_backup";
    Env::Default()->CreateDir(db_chroot);
    Env::Default()->CreateDir(backup_chroot);
    dbname_ = "/tempdb";
    backupdir_ = "/tempbk";

    // set up envs
    db_chroot_env_.reset(NewChrootEnv(Env::Default(), db_chroot));
    backup_chroot_env_.reset(NewChrootEnv(Env::Default(), backup_chroot));
    test_db_env_.reset(new TestEnv(db_chroot_env_.get()));
    test_backup_env_.reset(new TestEnv(backup_chroot_env_.get()));
    file_manager_.reset(new FileManager(backup_chroot_env_.get()));

    // set up db options
    options_.create_if_missing = true;
    options_.paranoid_checks = true;
    options_.write_buffer_size = 1 << 17; // 128KB
    options_.env = test_db_env_.get();
    options_.wal_dir = dbname_;

    // Create logger
    DBOptions logger_options;
    logger_options.env = db_chroot_env_.get();
    CreateLoggerFromOptions(dbname_, logger_options, &logger_);

    // set up backup db options
    backupable_options_.reset(new BackupableDBOptions(
        backupdir_, test_backup_env_.get(), true, logger_.get(), true));

    // most tests will use multi-threaded backups
    backupable_options_->max_background_operations = 7;

    // delete old files in db
    DestroyDB(dbname_, options_);
  }

  DB* OpenDB() {
    DB* db;
    EXPECT_OK(DB::Open(options_, dbname_, &db));
    return db;
  }

  void OpenDBAndBackupEngineShareWithChecksum(
      bool destroy_old_data = false, bool dummy = false,
      bool share_table_files = true, bool share_with_checksums = false) {
    backupable_options_->share_files_with_checksum = share_with_checksums;
    OpenDBAndBackupEngine(destroy_old_data, dummy, share_with_checksums);
  }

  void OpenDBAndBackupEngine(bool destroy_old_data = false, bool dummy = false,
                             bool share_table_files = true) {
    // reset all the defaults
    test_backup_env_->SetLimitWrittenFiles(1000000);
    test_db_env_->SetLimitWrittenFiles(1000000);
    test_db_env_->SetDummySequentialFile(dummy);

    DB* db;
    if (dummy) {
      dummy_db_ = new DummyDB(options_, dbname_);
      db = dummy_db_;
    } else {
      ASSERT_OK(DB::Open(options_, dbname_, &db));
    }
    db_.reset(db);
    backupable_options_->destroy_old_data = destroy_old_data;
    backupable_options_->share_table_files = share_table_files;
    BackupEngine* backup_engine;
    ASSERT_OK(BackupEngine::Open(test_db_env_.get(), *backupable_options_,
                                 &backup_engine));
    backup_engine_.reset(backup_engine);
  }

  void CloseDBAndBackupEngine() {
    db_.reset();
    backup_engine_.reset();
  }

  void OpenBackupEngine() {
    backupable_options_->destroy_old_data = false;
    BackupEngine* backup_engine;
    ASSERT_OK(BackupEngine::Open(test_db_env_.get(), *backupable_options_,
                                 &backup_engine));
    backup_engine_.reset(backup_engine);
  }

  void CloseBackupEngine() { backup_engine_.reset(nullptr); }

  // restores backup backup_id and asserts the existence of
  // [start_exist, end_exist> and not-existence of
  // [end_exist, end>
  //
  // if backup_id == 0, it means restore from latest
  // if end == 0, don't check AssertEmpty
  void AssertBackupConsistency(BackupID backup_id, uint32_t start_exist,
                               uint32_t end_exist, uint32_t end = 0,
                               bool keep_log_files = false) {
    RestoreOptions restore_options(keep_log_files);
    bool opened_backup_engine = false;
    if (backup_engine_.get() == nullptr) {
      opened_backup_engine = true;
      OpenBackupEngine();
    }
    if (backup_id > 0) {
      ASSERT_OK(backup_engine_->RestoreDBFromBackup(backup_id, dbname_, dbname_,
                                                    restore_options));
    } else {
      ASSERT_OK(backup_engine_->RestoreDBFromLatestBackup(dbname_, dbname_,
                                                          restore_options));
    }
    DB* db = OpenDB();
    AssertExists(db, start_exist, end_exist);
    if (end != 0) {
      AssertEmpty(db, end_exist, end);
    }
    delete db;
    if (opened_backup_engine) {
      CloseBackupEngine();
    }
  }

  void DeleteLogFiles() {
    std::vector<std::string> delete_logs;
    db_chroot_env_->GetChildren(dbname_, &delete_logs);
    for (auto f : delete_logs) {
      uint64_t number;
      FileType type;
      bool ok = ParseFileName(f, &number, &type);
      if (ok && type == kLogFile) {
        db_chroot_env_->DeleteFile(dbname_ + "/" + f);
      }
    }
  }

  // files
  std::string dbname_;
  std::string backupdir_;

  // logger_ must be above backup_engine_ such that the engine's destructor,
  // which uses a raw pointer to the logger, executes first.
  std::shared_ptr<Logger> logger_;

  // envs
  unique_ptr<Env> db_chroot_env_;
  unique_ptr<Env> backup_chroot_env_;
  unique_ptr<TestEnv> test_db_env_;
  unique_ptr<TestEnv> test_backup_env_;
  unique_ptr<FileManager> file_manager_;

  // all the dbs!
  DummyDB* dummy_db_; // BackupableDB owns dummy_db_
  unique_ptr<DB> db_;
  unique_ptr<BackupEngine> backup_engine_;

  // options
  Options options_;

 protected:
  unique_ptr<BackupableDBOptions> backupable_options_;
}; // BackupableDBTest

void AppendPath(const std::string& path, std::vector<std::string>& v) {
  for (auto& f : v) {
    f = path + f;
  }
}

class BackupableDBTestWithParam : public BackupableDBTest,
                                  public testing::WithParamInterface<bool> {
 public:
  BackupableDBTestWithParam() {
    backupable_options_->share_files_with_checksum = GetParam();
  }
};

// This test verifies that the verifyBackup method correctly identifies
// invalid backups
TEST_P(BackupableDBTestWithParam, VerifyBackup) {
  const int keys_iteration = 5000;
  Random rnd(6);
  Status s;
  OpenDBAndBackupEngine(true);
  // create five backups
  for (int i = 0; i < 5; ++i) {
    FillDB(db_.get(), keys_iteration * i, keys_iteration * (i + 1));
    ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), true));
  }
  CloseDBAndBackupEngine();

  OpenDBAndBackupEngine();
  // ---------- case 1. - valid backup -----------
  ASSERT_TRUE(backup_engine_->VerifyBackup(1).ok());

  // ---------- case 2. - delete a file -----------i
  file_manager_->DeleteRandomFileInDir(backupdir_ + "/private/1");
  ASSERT_TRUE(backup_engine_->VerifyBackup(1).IsNotFound());

  // ---------- case 3. - corrupt a file -----------
  std::string append_data = "Corrupting a random file";
  file_manager_->AppendToRandomFileInDir(backupdir_ + "/private/2",
                                         append_data);
  ASSERT_TRUE(backup_engine_->VerifyBackup(2).IsCorruption());

  // ---------- case 4. - invalid backup -----------
  ASSERT_TRUE(backup_engine_->VerifyBackup(6).IsNotFound());
  CloseDBAndBackupEngine();
}

// open DB, write, close DB, backup, restore, repeat
TEST_P(BackupableDBTestWithParam, OfflineIntegrationTest) {
  // has to be a big number, so that it triggers the memtable flush
  const int keys_iteration = 5000;
  const int max_key = keys_iteration * 4 + 10;
  // first iter -- flush before backup
  // second iter -- don't flush before backup
  for (int iter = 0; iter < 2; ++iter) {
    // delete old data
    DestroyDB(dbname_, options_);
    bool destroy_data = true;

    // every iteration --
    // 1. insert new data in the DB
    // 2. backup the DB
    // 3. destroy the db
    // 4. restore the db, check everything is still there
    for (int i = 0; i < 5; ++i) {
      // in last iteration, put smaller amount of data,
      int fill_up_to = std::min(keys_iteration * (i + 1), max_key);
      // ---- insert new data and back up ----
      OpenDBAndBackupEngine(destroy_data);
      destroy_data = false;
      FillDB(db_.get(), keys_iteration * i, fill_up_to);
      ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), iter == 0));
      CloseDBAndBackupEngine();
      DestroyDB(dbname_, options_);

      // ---- make sure it's empty ----
      DB* db = OpenDB();
      AssertEmpty(db, 0, fill_up_to);
      delete db;

      // ---- restore the DB ----
      OpenBackupEngine();
      if (i >= 3) {  // test purge old backups
        // when i == 4, purge to only 1 backup
        // when i == 3, purge to 2 backups
        ASSERT_OK(backup_engine_->PurgeOldBackups(5 - i));
      }
      // ---- make sure the data is there ---
      AssertBackupConsistency(0, 0, fill_up_to, max_key);
      CloseBackupEngine();
    }
  }
}

// open DB, write, backup, write, backup, close, restore
TEST_P(BackupableDBTestWithParam, OnlineIntegrationTest) {
  // has to be a big number, so that it triggers the memtable flush
  const int keys_iteration = 5000;
  const int max_key = keys_iteration * 4 + 10;
  Random rnd(7);
  // delete old data
  DestroyDB(dbname_, options_);

  OpenDBAndBackupEngine(true);
  // write some data, backup, repeat
  for (int i = 0; i < 5; ++i) {
    if (i == 4) {
      // delete backup number 2, online delete!
      ASSERT_OK(backup_engine_->DeleteBackup(2));
    }
    // in last iteration, put smaller amount of data,
    // so that backups can share sst files
    int fill_up_to = std::min(keys_iteration * (i + 1), max_key);
    FillDB(db_.get(), keys_iteration * i, fill_up_to);
    // we should get consistent results with flush_before_backup
    // set to both true and false
    ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), !!(rnd.Next() % 2)));
  }
  // close and destroy
  CloseDBAndBackupEngine();
  DestroyDB(dbname_, options_);

  // ---- make sure it's empty ----
  DB* db = OpenDB();
  AssertEmpty(db, 0, max_key);
  delete db;

  // ---- restore every backup and verify all the data is there ----
  OpenBackupEngine();
  for (int i = 1; i <= 5; ++i) {
    if (i == 2) {
      // we deleted backup 2
      Status s = backup_engine_->RestoreDBFromBackup(2, dbname_, dbname_);
      ASSERT_TRUE(!s.ok());
    } else {
      int fill_up_to = std::min(keys_iteration * i, max_key);
      AssertBackupConsistency(i, 0, fill_up_to, max_key);
    }
  }

  // delete some backups -- this should leave only backups 3 and 5 alive
  ASSERT_OK(backup_engine_->DeleteBackup(4));
  ASSERT_OK(backup_engine_->PurgeOldBackups(2));

  std::vector<BackupInfo> backup_info;
  backup_engine_->GetBackupInfo(&backup_info);
  ASSERT_EQ(2UL, backup_info.size());

  // check backup 3
  AssertBackupConsistency(3, 0, 3 * keys_iteration, max_key);
  // check backup 5
  AssertBackupConsistency(5, 0, max_key);

  CloseBackupEngine();
}

INSTANTIATE_TEST_CASE_P(BackupableDBTestWithParam, BackupableDBTestWithParam,
                        ::testing::Bool());

// this will make sure that backup does not copy the same file twice
TEST_F(BackupableDBTest, NoDoubleCopy) {
  OpenDBAndBackupEngine(true, true);

  // should write 5 DB files + one meta file
  test_backup_env_->SetLimitWrittenFiles(7);
  test_backup_env_->ClearWrittenFiles();
  test_db_env_->SetLimitWrittenFiles(0);
  dummy_db_->live_files_ = {"/00010.sst", "/00011.sst", "/CURRENT",
                            "/MANIFEST-01"};
  dummy_db_->wal_files_ = {{"/00011.log", true}, {"/00012.log", false}};
  test_db_env_->SetFilenamesForMockedAttrs(dummy_db_->live_files_);
  ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), false));
  std::vector<std::string> should_have_written = {
      "/shared/00010.sst.tmp",    "/shared/00011.sst.tmp",
      "/private/1.tmp/CURRENT",   "/private/1.tmp/MANIFEST-01",
      "/private/1.tmp/00011.log", "/meta/1.tmp"};
  AppendPath(backupdir_, should_have_written);
  test_backup_env_->AssertWrittenFiles(should_have_written);

  // should write 4 new DB files + one meta file
  // should not write/copy 00010.sst, since it's already there!
  test_backup_env_->SetLimitWrittenFiles(6);
  test_backup_env_->ClearWrittenFiles();

  dummy_db_->live_files_ = {"/00010.sst", "/00015.sst", "/CURRENT",
                            "/MANIFEST-01"};
  dummy_db_->wal_files_ = {{"/00011.log", true}, {"/00012.log", false}};
  test_db_env_->SetFilenamesForMockedAttrs(dummy_db_->live_files_);
  ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), false));
  // should not open 00010.sst - it's already there

  should_have_written = {"/shared/00015.sst.tmp", "/private/2.tmp/CURRENT",
                         "/private/2.tmp/MANIFEST-01",
                         "/private/2.tmp/00011.log", "/meta/2.tmp"};
  AppendPath(backupdir_, should_have_written);
  test_backup_env_->AssertWrittenFiles(should_have_written);

  ASSERT_OK(backup_engine_->DeleteBackup(1));
  ASSERT_OK(test_backup_env_->FileExists(backupdir_ + "/shared/00010.sst"));

  // 00011.sst was only in backup 1, should be deleted
  ASSERT_EQ(Status::NotFound(),
            test_backup_env_->FileExists(backupdir_ + "/shared/00011.sst"));
  ASSERT_OK(test_backup_env_->FileExists(backupdir_ + "/shared/00015.sst"));

  // MANIFEST file size should be only 100
  uint64_t size;
  test_backup_env_->GetFileSize(backupdir_ + "/private/2/MANIFEST-01", &size);
  ASSERT_EQ(100UL, size);
  test_backup_env_->GetFileSize(backupdir_ + "/shared/00015.sst", &size);
  ASSERT_EQ(200UL, size);

  CloseDBAndBackupEngine();
}

// test various kind of corruptions that may happen:
// 1. Not able to write a file for backup - that backup should fail,
//      everything else should work
// 2. Corrupted backup meta file or missing backuped file - we should
//      not be able to open that backup, but all other backups should be
//      fine
// 3. Corrupted checksum value - if the checksum is not a valid uint32_t,
//      db open should fail, otherwise, it aborts during the restore process.
TEST_F(BackupableDBTest, CorruptionsTest) {
  const int keys_iteration = 5000;
  Random rnd(6);
  Status s;

  OpenDBAndBackupEngine(true);
  // create five backups
  for (int i = 0; i < 5; ++i) {
    FillDB(db_.get(), keys_iteration * i, keys_iteration * (i + 1));
    ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), !!(rnd.Next() % 2)));
  }

  // ---------- case 1. - fail a write -----------
  // try creating backup 6, but fail a write
  FillDB(db_.get(), keys_iteration * 5, keys_iteration * 6);
  test_backup_env_->SetLimitWrittenFiles(2);
  // should fail
  s = backup_engine_->CreateNewBackup(db_.get(), !!(rnd.Next() % 2));
  ASSERT_TRUE(!s.ok());
  test_backup_env_->SetLimitWrittenFiles(1000000);
  // latest backup should have all the keys
  CloseDBAndBackupEngine();
  AssertBackupConsistency(0, 0, keys_iteration * 5, keys_iteration * 6);

  // --------- case 2. corrupted backup meta or missing backuped file ----
  ASSERT_OK(file_manager_->CorruptFile(backupdir_ + "/meta/5", 3));
  // since 5 meta is now corrupted, latest backup should be 4
  AssertBackupConsistency(0, 0, keys_iteration * 4, keys_iteration * 5);
  OpenBackupEngine();
  s = backup_engine_->RestoreDBFromBackup(5, dbname_, dbname_);
  ASSERT_TRUE(!s.ok());
  CloseBackupEngine();
  ASSERT_OK(file_manager_->DeleteRandomFileInDir(backupdir_ + "/private/4"));
  // 4 is corrupted, 3 is the latest backup now
  AssertBackupConsistency(0, 0, keys_iteration * 3, keys_iteration * 5);
  OpenBackupEngine();
  s = backup_engine_->RestoreDBFromBackup(4, dbname_, dbname_);
  CloseBackupEngine();
  ASSERT_TRUE(!s.ok());

  // --------- case 3. corrupted checksum value ----
  ASSERT_OK(file_manager_->CorruptChecksum(backupdir_ + "/meta/3", false));
  // checksum of backup 3 is an invalid value, this can be detected at
  // db open time, and it reverts to the previous backup automatically
  AssertBackupConsistency(0, 0, keys_iteration * 2, keys_iteration * 5);
  // checksum of the backup 2 appears to be valid, this can cause checksum
  // mismatch and abort restore process
  ASSERT_OK(file_manager_->CorruptChecksum(backupdir_ + "/meta/2", true));
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/meta/2"));
  OpenBackupEngine();
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/meta/2"));
  s = backup_engine_->RestoreDBFromBackup(2, dbname_, dbname_);
  ASSERT_TRUE(!s.ok());

  // make sure that no corrupt backups have actually been deleted!
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/meta/1"));
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/meta/2"));
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/meta/3"));
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/meta/4"));
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/meta/5"));
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/private/1"));
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/private/2"));
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/private/3"));
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/private/4"));
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/private/5"));

  // delete the corrupt backups and then make sure they're actually deleted
  ASSERT_OK(backup_engine_->DeleteBackup(5));
  ASSERT_OK(backup_engine_->DeleteBackup(4));
  ASSERT_OK(backup_engine_->DeleteBackup(3));
  ASSERT_OK(backup_engine_->DeleteBackup(2));
  (void)backup_engine_->GarbageCollect();
  ASSERT_EQ(Status::NotFound(),
            file_manager_->FileExists(backupdir_ + "/meta/5"));
  ASSERT_EQ(Status::NotFound(),
            file_manager_->FileExists(backupdir_ + "/private/5"));
  ASSERT_EQ(Status::NotFound(),
            file_manager_->FileExists(backupdir_ + "/meta/4"));
  ASSERT_EQ(Status::NotFound(),
            file_manager_->FileExists(backupdir_ + "/private/4"));
  ASSERT_EQ(Status::NotFound(),
            file_manager_->FileExists(backupdir_ + "/meta/3"));
  ASSERT_EQ(Status::NotFound(),
            file_manager_->FileExists(backupdir_ + "/private/3"));
  ASSERT_EQ(Status::NotFound(),
            file_manager_->FileExists(backupdir_ + "/meta/2"));
  ASSERT_EQ(Status::NotFound(),
            file_manager_->FileExists(backupdir_ + "/private/2"));

  CloseBackupEngine();
  AssertBackupConsistency(0, 0, keys_iteration * 1, keys_iteration * 5);

  // new backup should be 2!
  OpenDBAndBackupEngine();
  FillDB(db_.get(), keys_iteration * 1, keys_iteration * 2);
  ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), !!(rnd.Next() % 2)));
  CloseDBAndBackupEngine();
  AssertBackupConsistency(2, 0, keys_iteration * 2, keys_iteration * 5);
}

TEST_F(BackupableDBTest, InterruptCreationTest) {
  // Interrupt backup creation by failing new writes and failing cleanup of the
  // partial state. Then verify a subsequent backup can still succeed.
  const int keys_iteration = 5000;
  Random rnd(6);

  OpenDBAndBackupEngine(true /* destroy_old_data */);
  FillDB(db_.get(), 0, keys_iteration);
  test_backup_env_->SetLimitWrittenFiles(2);
  test_backup_env_->SetDeleteFileFailure(true);
  // should fail creation
  ASSERT_FALSE(
      backup_engine_->CreateNewBackup(db_.get(), !!(rnd.Next() % 2)).ok());
  CloseDBAndBackupEngine();
  // should also fail cleanup so the tmp directory stays behind
  ASSERT_OK(backup_chroot_env_->FileExists(backupdir_ + "/private/1.tmp/"));

  OpenDBAndBackupEngine(false /* destroy_old_data */);
  test_backup_env_->SetLimitWrittenFiles(1000000);
  test_backup_env_->SetDeleteFileFailure(false);
  ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), !!(rnd.Next() % 2)));
  // latest backup should have all the keys
  CloseDBAndBackupEngine();
  AssertBackupConsistency(0, 0, keys_iteration);
}

inline std::string OptionsPath(std::string ret, int backupID) {
  ret += "/private/";
  ret += std::to_string(backupID);
  ret += "/";
  return ret;
}

// Backup the LATEST options file to
// "<backup_dir>/private/<backup_id>/OPTIONS<number>"

TEST_F(BackupableDBTest, BackupOptions) {
  OpenDBAndBackupEngine(true);
  for (int i = 1; i < 5; i++) {
    std::string name;
    std::vector<std::string> filenames;
    // Must reset() before reset(OpenDB()) again.
    // Calling OpenDB() while *db_ is existing will cause LOCK issue
    db_.reset();
    db_.reset(OpenDB());
    ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), true));
    rocksdb::GetLatestOptionsFileName(db_->GetName(), options_.env, &name);
    ASSERT_OK(file_manager_->FileExists(OptionsPath(backupdir_, i) + name));
    backup_chroot_env_->GetChildren(OptionsPath(backupdir_, i), &filenames);
    for (auto fn : filenames) {
      if (fn.compare(0, 7, "OPTIONS") == 0) {
        ASSERT_EQ(name, fn);
      }
    }
  }

  CloseDBAndBackupEngine();
}

// This test verifies we don't delete the latest backup when read-only option is
// set
TEST_F(BackupableDBTest, NoDeleteWithReadOnly) {
  const int keys_iteration = 5000;
  Random rnd(6);
  Status s;

  OpenDBAndBackupEngine(true);
  // create five backups
  for (int i = 0; i < 5; ++i) {
    FillDB(db_.get(), keys_iteration * i, keys_iteration * (i + 1));
    ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), !!(rnd.Next() % 2)));
  }
  CloseDBAndBackupEngine();
  ASSERT_OK(file_manager_->WriteToFile(backupdir_ + "/LATEST_BACKUP", "4"));

  backupable_options_->destroy_old_data = false;
  BackupEngineReadOnly* read_only_backup_engine;
  ASSERT_OK(BackupEngineReadOnly::Open(backup_chroot_env_.get(),
                                       *backupable_options_,
                                       &read_only_backup_engine));

  // assert that data from backup 5 is still here (even though LATEST_BACKUP
  // says 4 is latest)
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/meta/5"));
  ASSERT_OK(file_manager_->FileExists(backupdir_ + "/private/5"));

  // Behavior change: We now ignore LATEST_BACKUP contents. This means that
  // we should have 5 backups, even if LATEST_BACKUP says 4.
  std::vector<BackupInfo> backup_info;
  read_only_backup_engine->GetBackupInfo(&backup_info);
  ASSERT_EQ(5UL, backup_info.size());
  delete read_only_backup_engine;
}

TEST_F(BackupableDBTest, FailOverwritingBackups) {
  options_.write_buffer_size = 1024 * 1024 * 1024;  // 1GB
  options_.disable_auto_compactions = true;

  // create backups 1, 2, 3, 4, 5
  OpenDBAndBackupEngine(true);
  for (int i = 0; i < 5; ++i) {
    CloseDBAndBackupEngine();
    DeleteLogFiles();
    OpenDBAndBackupEngine(false);
    FillDB(db_.get(), 100 * i, 100 * (i + 1));
    ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), true));
  }
  CloseDBAndBackupEngine();

  // restore 3
  OpenBackupEngine();
  ASSERT_OK(backup_engine_->RestoreDBFromBackup(3, dbname_, dbname_));
  CloseBackupEngine();

  OpenDBAndBackupEngine(false);
  FillDB(db_.get(), 0, 300);
  Status s = backup_engine_->CreateNewBackup(db_.get(), true);
  // the new backup fails because new table files
  // clash with old table files from backups 4 and 5
  // (since write_buffer_size is huge, we can be sure that
  // each backup will generate only one sst file and that
  // a file generated by a new backup is the same as
  // sst file generated by backup 4)
  ASSERT_TRUE(s.IsCorruption());
  ASSERT_OK(backup_engine_->DeleteBackup(4));
  ASSERT_OK(backup_engine_->DeleteBackup(5));
  // now, the backup can succeed
  ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), true));
  CloseDBAndBackupEngine();
}

TEST_F(BackupableDBTest, NoShareTableFiles) {
  const int keys_iteration = 5000;
  OpenDBAndBackupEngine(true, false, false);
  for (int i = 0; i < 5; ++i) {
    FillDB(db_.get(), keys_iteration * i, keys_iteration * (i + 1));
    ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), !!(i % 2)));
  }
  CloseDBAndBackupEngine();

  for (int i = 0; i < 5; ++i) {
    AssertBackupConsistency(i + 1, 0, keys_iteration * (i + 1),
                            keys_iteration * 6);
  }
}

// Verify that you can backup and restore with share_files_with_checksum on
TEST_F(BackupableDBTest, ShareTableFilesWithChecksums) {
  const int keys_iteration = 5000;
  OpenDBAndBackupEngineShareWithChecksum(true, false, true, true);
  for (int i = 0; i < 5; ++i) {
    FillDB(db_.get(), keys_iteration * i, keys_iteration * (i + 1));
    ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), !!(i % 2)));
  }
  CloseDBAndBackupEngine();

  for (int i = 0; i < 5; ++i) {
    AssertBackupConsistency(i + 1, 0, keys_iteration * (i + 1),
                            keys_iteration * 6);
  }
}

// Verify that you can backup and restore using share_files_with_checksum set to
// false and then transition this option to true
TEST_F(BackupableDBTest, ShareTableFilesWithChecksumsTransition) {
  const int keys_iteration = 5000;
  // set share_files_with_checksum to false
  OpenDBAndBackupEngineShareWithChecksum(true, false, true, false);
  for (int i = 0; i < 5; ++i) {
    FillDB(db_.get(), keys_iteration * i, keys_iteration * (i + 1));
    ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), true));
  }
  CloseDBAndBackupEngine();

  for (int i = 0; i < 5; ++i) {
    AssertBackupConsistency(i + 1, 0, keys_iteration * (i + 1),
                            keys_iteration * 6);
  }

  // set share_files_with_checksum to true and do some more backups
  OpenDBAndBackupEngineShareWithChecksum(true, false, true, true);
  for (int i = 5; i < 10; ++i) {
    FillDB(db_.get(), keys_iteration * i, keys_iteration * (i + 1));
    ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), true));
  }
  CloseDBAndBackupEngine();

  for (int i = 0; i < 5; ++i) {
    AssertBackupConsistency(i + 1, 0, keys_iteration * (i + 5 + 1),
                            keys_iteration * 11);
  }
}

TEST_F(BackupableDBTest, DeleteTmpFiles) {
  OpenDBAndBackupEngine();
  CloseDBAndBackupEngine();
  std::string shared_tmp = backupdir_ + "/shared/00006.sst.tmp";
  std::string private_tmp_dir = backupdir_ + "/private/10.tmp";
  std::string private_tmp_file = private_tmp_dir + "/00003.sst";
  file_manager_->WriteToFile(shared_tmp, "tmp");
  file_manager_->CreateDir(private_tmp_dir);
  file_manager_->WriteToFile(private_tmp_file, "tmp");
  ASSERT_OK(file_manager_->FileExists(private_tmp_dir));
  OpenDBAndBackupEngine();
  // Need to call this explicitly to delete tmp files
  (void)backup_engine_->GarbageCollect();
  CloseDBAndBackupEngine();
  ASSERT_EQ(Status::NotFound(), file_manager_->FileExists(shared_tmp));
  ASSERT_EQ(Status::NotFound(), file_manager_->FileExists(private_tmp_file));
  ASSERT_EQ(Status::NotFound(), file_manager_->FileExists(private_tmp_dir));
}

TEST_F(BackupableDBTest, KeepLogFiles) {
  backupable_options_->backup_log_files = false;
  // basically infinite
  options_.WAL_ttl_seconds = 24 * 60 * 60;
  OpenDBAndBackupEngine(true);
  FillDB(db_.get(), 0, 100);
  ASSERT_OK(db_->Flush(FlushOptions()));
  FillDB(db_.get(), 100, 200);
  ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), false));
  FillDB(db_.get(), 200, 300);
  ASSERT_OK(db_->Flush(FlushOptions()));
  FillDB(db_.get(), 300, 400);
  ASSERT_OK(db_->Flush(FlushOptions()));
  FillDB(db_.get(), 400, 500);
  ASSERT_OK(db_->Flush(FlushOptions()));
  CloseDBAndBackupEngine();

  // all data should be there if we call with keep_log_files = true
  AssertBackupConsistency(0, 0, 500, 600, true);
}

TEST_F(BackupableDBTest, RateLimiting) {
  size_t const kMicrosPerSec = 1000 * 1000LL;
  uint64_t const MB = 1024 * 1024;

  const std::vector<std::pair<uint64_t, uint64_t>> limits(
      {{1 * MB, 5 * MB}, {2 * MB, 3 * MB}});

  std::shared_ptr<RateLimiter> backupThrottler(NewGenericRateLimiter(1));
  std::shared_ptr<RateLimiter> restoreThrottler(NewGenericRateLimiter(1));

  for (bool makeThrottler : {false, true}) {
    if (makeThrottler) {
      backupable_options_->backup_rate_limiter = backupThrottler;
      backupable_options_->restore_rate_limiter = restoreThrottler;
    }
    // iter 0 -- single threaded
    // iter 1 -- multi threaded
    for (int iter = 0; iter < 2; ++iter) {
      for (const auto& limit : limits) {
        // destroy old data
        DestroyDB(dbname_, Options());
        if (makeThrottler) {
          backupThrottler->SetBytesPerSecond(limit.first);
          restoreThrottler->SetBytesPerSecond(limit.second);
        } else {
          backupable_options_->backup_rate_limit = limit.first;
          backupable_options_->restore_rate_limit = limit.second;
        }
        backupable_options_->max_background_operations = (iter == 0) ? 1 : 10;
        options_.compression = kNoCompression;
        OpenDBAndBackupEngine(true);
        size_t bytes_written = FillDB(db_.get(), 0, 100000);

        auto start_backup = db_chroot_env_->NowMicros();
        ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), false));
        auto backup_time = db_chroot_env_->NowMicros() - start_backup;
        auto rate_limited_backup_time =
            (bytes_written * kMicrosPerSec) / limit.first;
        ASSERT_GT(backup_time, 0.8 * rate_limited_backup_time);

        CloseDBAndBackupEngine();

        OpenBackupEngine();
        auto start_restore = db_chroot_env_->NowMicros();
        ASSERT_OK(backup_engine_->RestoreDBFromLatestBackup(dbname_, dbname_));
        auto restore_time = db_chroot_env_->NowMicros() - start_restore;
        CloseBackupEngine();
        auto rate_limited_restore_time =
            (bytes_written * kMicrosPerSec) / limit.second;
        ASSERT_GT(restore_time, 0.8 * rate_limited_restore_time);

        AssertBackupConsistency(0, 0, 100000, 100010);
      }
    }
  }
}

TEST_F(BackupableDBTest, ReadOnlyBackupEngine) {
  DestroyDB(dbname_, options_);
  OpenDBAndBackupEngine(true);
  FillDB(db_.get(), 0, 100);
  ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), true));
  FillDB(db_.get(), 100, 200);
  ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), true));
  CloseDBAndBackupEngine();
  DestroyDB(dbname_, options_);

  backupable_options_->destroy_old_data = false;
  test_backup_env_->ClearWrittenFiles();
  test_backup_env_->SetLimitDeleteFiles(0);
  BackupEngineReadOnly* read_only_backup_engine;
  ASSERT_OK(BackupEngineReadOnly::Open(
      db_chroot_env_.get(), *backupable_options_, &read_only_backup_engine));
  std::vector<BackupInfo> backup_info;
  read_only_backup_engine->GetBackupInfo(&backup_info);
  ASSERT_EQ(backup_info.size(), 2U);

  RestoreOptions restore_options(false);
  ASSERT_OK(read_only_backup_engine->RestoreDBFromLatestBackup(
      dbname_, dbname_, restore_options));
  delete read_only_backup_engine;
  std::vector<std::string> should_have_written;
  test_backup_env_->AssertWrittenFiles(should_have_written);

  DB* db = OpenDB();
  AssertExists(db, 0, 200);
  delete db;
}

TEST_F(BackupableDBTest, ProgressCallbackDuringBackup) {
  DestroyDB(dbname_, options_);
  OpenDBAndBackupEngine(true);
  FillDB(db_.get(), 0, 100);
  bool is_callback_invoked = false;
  ASSERT_OK(backup_engine_->CreateNewBackup(
      db_.get(), true,
      [&is_callback_invoked]() { is_callback_invoked = true; }));

  ASSERT_TRUE(is_callback_invoked);
  CloseDBAndBackupEngine();
  DestroyDB(dbname_, options_);
}

TEST_F(BackupableDBTest, GarbageCollectionBeforeBackup) {
  DestroyDB(dbname_, options_);
  OpenDBAndBackupEngine(true);

  backup_chroot_env_->CreateDirIfMissing(backupdir_ + "/shared");
  std::string file_five = backupdir_ + "/shared/000007.sst";
  std::string file_five_contents = "I'm not really a sst file";
  // this depends on the fact that 00007.sst is the first file created by the DB
  ASSERT_OK(file_manager_->WriteToFile(file_five, file_five_contents));

  FillDB(db_.get(), 0, 100);
  // backup overwrites file 000007.sst
  ASSERT_TRUE(backup_engine_->CreateNewBackup(db_.get(), true).ok());

  std::string new_file_five_contents;
  ASSERT_OK(ReadFileToString(backup_chroot_env_.get(), file_five,
                             &new_file_five_contents));
  // file 000007.sst was overwritten
  ASSERT_TRUE(new_file_five_contents != file_five_contents);

  CloseDBAndBackupEngine();

  AssertBackupConsistency(0, 0, 100);
}

// Test that we properly propagate Env failures
TEST_F(BackupableDBTest, EnvFailures) {
  BackupEngine* backup_engine;

  // get children failure
  {
    test_backup_env_->SetGetChildrenFailure(true);
    ASSERT_NOK(BackupEngine::Open(test_db_env_.get(), *backupable_options_,
                                  &backup_engine));
    test_backup_env_->SetGetChildrenFailure(false);
  }

  // created dir failure
  {
    test_backup_env_->SetCreateDirIfMissingFailure(true);
    ASSERT_NOK(BackupEngine::Open(test_db_env_.get(), *backupable_options_,
                                  &backup_engine));
    test_backup_env_->SetCreateDirIfMissingFailure(false);
  }

  // new directory failure
  {
    test_backup_env_->SetNewDirectoryFailure(true);
    ASSERT_NOK(BackupEngine::Open(test_db_env_.get(), *backupable_options_,
                                  &backup_engine));
    test_backup_env_->SetNewDirectoryFailure(false);
  }

  // Read from meta-file failure
  {
    DestroyDB(dbname_, options_);
    OpenDBAndBackupEngine(true);
    FillDB(db_.get(), 0, 100);
    ASSERT_TRUE(backup_engine_->CreateNewBackup(db_.get(), true).ok());
    CloseDBAndBackupEngine();
    test_backup_env_->SetDummySequentialFile(true);
    test_backup_env_->SetDummySequentialFileFailReads(true);
    backupable_options_->destroy_old_data = false;
    ASSERT_NOK(BackupEngine::Open(test_db_env_.get(), *backupable_options_,
                                  &backup_engine));
    test_backup_env_->SetDummySequentialFile(false);
    test_backup_env_->SetDummySequentialFileFailReads(false);
  }

  // no failure
  {
    ASSERT_OK(BackupEngine::Open(test_db_env_.get(), *backupable_options_,
                                 &backup_engine));
    delete backup_engine;
  }
}

// Verify manifest can roll while a backup is being created with the old
// manifest.
TEST_F(BackupableDBTest, ChangeManifestDuringBackupCreation) {
  DestroyDB(dbname_, options_);
  options_.max_manifest_file_size = 0;  // always rollover manifest for file add
  OpenDBAndBackupEngine(true);
  FillDB(db_.get(), 0, 100);

  rocksdb::SyncPoint::GetInstance()->LoadDependency({
      {"BackupEngineImpl::CreateNewBackup:SavedLiveFiles1",
       "VersionSet::LogAndApply:WriteManifest"},
      {"VersionSet::LogAndApply:WriteManifestDone",
       "BackupEngineImpl::CreateNewBackup:SavedLiveFiles2"},
  });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  rocksdb::port::Thread flush_thread{[this]() { ASSERT_OK(db_->Flush(FlushOptions())); }};

  ASSERT_OK(backup_engine_->CreateNewBackup(db_.get(), false));

  flush_thread.join();
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();

  // The last manifest roll would've already been cleaned up by the full scan
  // that happens when CreateNewBackup invokes EnableFileDeletions. We need to
  // trigger another roll to verify non-full scan purges stale manifests.
  DBImpl* db_impl = reinterpret_cast<DBImpl*>(db_.get());
  std::string prev_manifest_path =
      DescriptorFileName(dbname_, db_impl->TEST_Current_Manifest_FileNo());
  FillDB(db_.get(), 0, 100);
  ASSERT_OK(db_chroot_env_->FileExists(prev_manifest_path));
  ASSERT_OK(db_->Flush(FlushOptions()));
  ASSERT_TRUE(db_chroot_env_->FileExists(prev_manifest_path).IsNotFound());

  CloseDBAndBackupEngine();
  DestroyDB(dbname_, options_);
  AssertBackupConsistency(0, 0, 100);
}

// see https://github.com/facebook/rocksdb/issues/921
TEST_F(BackupableDBTest, Issue921Test) {
  BackupEngine* backup_engine;
  backupable_options_->share_table_files = false;
  backup_chroot_env_->CreateDirIfMissing(backupable_options_->backup_dir);
  backupable_options_->backup_dir += "/new_dir";
  ASSERT_OK(BackupEngine::Open(backup_chroot_env_.get(), *backupable_options_,
                               &backup_engine));

  delete backup_engine;
}

TEST_F(BackupableDBTest, BackupWithMetadata) {
  const int keys_iteration = 5000;
  OpenDBAndBackupEngine(true);
  // create five backups
  for (int i = 0; i < 5; ++i) {
    const std::string metadata = std::to_string(i);
    FillDB(db_.get(), keys_iteration * i, keys_iteration * (i + 1));
    ASSERT_OK(
        backup_engine_->CreateNewBackupWithMetadata(db_.get(), metadata, true));
  }
  CloseDBAndBackupEngine();

  OpenDBAndBackupEngine();
  std::vector<BackupInfo> backup_infos;
  backup_engine_->GetBackupInfo(&backup_infos);
  ASSERT_EQ(5, backup_infos.size());
  for (int i = 0; i < 5; i++) {
    ASSERT_EQ(std::to_string(i), backup_infos[i].app_metadata);
  }
  CloseDBAndBackupEngine();
  DestroyDB(dbname_, options_);
}

TEST_F(BackupableDBTest, BinaryMetadata) {
  OpenDBAndBackupEngine(true);
  std::string binaryMetadata = "abc\ndef";
  binaryMetadata.push_back('\0');
  binaryMetadata.append("ghi");
  ASSERT_OK(
      backup_engine_->CreateNewBackupWithMetadata(db_.get(), binaryMetadata));
  CloseDBAndBackupEngine();

  OpenDBAndBackupEngine();
  std::vector<BackupInfo> backup_infos;
  backup_engine_->GetBackupInfo(&backup_infos);
  ASSERT_EQ(1, backup_infos.size());
  ASSERT_EQ(binaryMetadata, backup_infos[0].app_metadata);
  CloseDBAndBackupEngine();
  DestroyDB(dbname_, options_);
}

TEST_F(BackupableDBTest, MetadataTooLarge) {
  OpenDBAndBackupEngine(true);
  std::string largeMetadata(1024 * 1024 + 1, 0);
  ASSERT_NOK(
      backup_engine_->CreateNewBackupWithMetadata(db_.get(), largeMetadata));
  CloseDBAndBackupEngine();
  DestroyDB(dbname_, options_);
}
}  // anon namespace

} //  namespace rocksdb

int main(int argc, char** argv) {
  rocksdb::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else
#include <stdio.h>

int main(int argc, char** argv) {
  fprintf(stderr, "SKIPPED as BackupableDB is not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // !defined(ROCKSDB_LITE) && !defined(OS_WIN)
