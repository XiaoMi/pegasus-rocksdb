//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2012 Facebook.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ROCKSDB_LITE

#include "utilities/checkpoint/checkpoint_impl.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <algorithm>
#include <string>
#include <vector>

#include "db/log_writer.h"
#include "db/wal_manager.h"
#include "port/port.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/transaction_log.h"
#include "rocksdb/utilities/checkpoint.h"
#include "util/coding.h"
#include "util/file_reader_writer.h"
#include "util/file_util.h"
#include "util/filename.h"
#include "util/sync_point.h"

namespace rocksdb {

Status Checkpoint::Create(DB* db, Checkpoint** checkpoint_ptr) {
  *checkpoint_ptr = new CheckpointImpl(db);
  return Status::OK();
}

Status Checkpoint::CreateCheckpoint(const std::string& checkpoint_dir,
                                    uint64_t log_size_for_flush) {
  return Status::NotSupported("");
}

Status Checkpoint::CreateCheckpointQuick(const std::string& checkpoint_dir,
                                         uint64_t* checkpoint_decree) {
  return Status::NotSupported("");
}

// Builds an openable snapshot of RocksDB
Status CheckpointImpl::CreateCheckpoint(const std::string& checkpoint_dir,
                                        uint64_t log_size_for_flush) {
  DBOptions db_options = db_->GetDBOptions();
  // ATTENTION(laiyingchun): log_size_for_flush should always be 0 in pegasus.
  assert(!db_options.pegasus_data || log_size_for_flush == 0);

  Status s = db_->GetEnv()->FileExists(checkpoint_dir);
  if (s.ok()) {
    return Status::InvalidArgument("Directory exists");
  } else if (!s.IsNotFound()) {
    assert(s.IsIOError());
    return s;
  }

  ROCKS_LOG_INFO(
      db_options.info_log,
      "Started the snapshot process -- creating snapshot in directory %s",
      checkpoint_dir.c_str());
  std::string full_private_path = checkpoint_dir + ".tmp";
  // create snapshot directory
  s = db_->GetEnv()->CreateDir(full_private_path);
  uint64_t sequence_number = 0;
  if (s.ok()) {
    db_->DisableFileDeletions();
    s = CreateCustomCheckpoint(
        db_options,
        [&](const std::string& src_dirname, const std::string& fname,
            FileType) {
          ROCKS_LOG_INFO(db_options.info_log, "Hard Linking %s", fname.c_str());
          return db_->GetEnv()->LinkFile(src_dirname + fname,
                                         full_private_path + fname);
        } /* link_file_cb */,
        [&](const std::string& src_dirname, const std::string& fname,
            uint64_t size_limit_bytes, FileType) {
          ROCKS_LOG_INFO(db_options.info_log, "Copying %s", fname.c_str());
          return CopyFile(db_->GetEnv(), src_dirname + fname,
                          full_private_path + fname, size_limit_bytes,
                          db_options.use_fsync);
        } /* copy_file_cb */,
        [&](const std::string& fname, const std::string& contents, FileType) {
          ROCKS_LOG_INFO(db_options.info_log, "Creating %s", fname.c_str());
          return CreateFile(db_->GetEnv(), full_private_path + fname, contents);
        } /* create_file_cb */,
        &sequence_number, log_size_for_flush);
    // we copied all the files, enable file deletions
    db_->EnableFileDeletions(false);
  }

  if (s.ok()) {
    // move tmp private backup to real snapshot directory
    s = db_->GetEnv()->RenameFile(full_private_path, checkpoint_dir);
  }
  if (s.ok()) {
    unique_ptr<Directory> checkpoint_directory;
    db_->GetEnv()->NewDirectory(checkpoint_dir, &checkpoint_directory);
    if (checkpoint_directory != nullptr) {
      s = checkpoint_directory->Fsync();
    }
  }

  if (s.ok()) {
    // here we know that we succeeded and installed the new snapshot
    ROCKS_LOG_INFO(db_options.info_log, "Snapshot DONE. All is good");
    ROCKS_LOG_INFO(db_options.info_log, "Snapshot sequence number: %" PRIu64,
                   sequence_number);
  } else {
    // clean all the files we might have created
    ROCKS_LOG_INFO(db_options.info_log, "Snapshot failed -- %s",
                   s.ToString().c_str());
    // we have to delete the dir and all its children
    std::vector<std::string> subchildren;
    db_->GetEnv()->GetChildren(full_private_path, &subchildren);
    for (auto& subchild : subchildren) {
      std::string subchild_path = full_private_path + "/" + subchild;
      Status s1 = db_->GetEnv()->DeleteFile(subchild_path);
      ROCKS_LOG_INFO(db_options.info_log, "Delete file %s -- %s",
                     subchild_path.c_str(), s1.ToString().c_str());
    }
    // finally delete the private dir
    Status s1 = db_->GetEnv()->DeleteDir(full_private_path);
    ROCKS_LOG_INFO(db_options.info_log, "Delete dir %s -- %s",
                   full_private_path.c_str(), s1.ToString().c_str());
  }
  return s;
}

Status CheckpointImpl::CreateCustomCheckpoint(
    const DBOptions& db_options,
    std::function<Status(const std::string& src_dirname,
                         const std::string& src_fname, FileType type)>
        link_file_cb,
    std::function<Status(const std::string& src_dirname,
                         const std::string& src_fname,
                         uint64_t size_limit_bytes, FileType type)>
        copy_file_cb,
    std::function<Status(const std::string& fname, const std::string& contents,
                         FileType type)>
        create_file_cb,
    uint64_t* sequence_number, uint64_t log_size_for_flush) {
  Status s;
  std::vector<std::string> live_files;
  uint64_t manifest_file_size = 0;
  uint64_t min_log_num = port::kMaxUint64;
  *sequence_number = db_->GetLatestSequenceNumber();
  bool same_fs = true;
  VectorLogPtr live_wal_files;

  bool flush_memtable = true;
  if (s.ok()) {
    if (!db_options.allow_2pc) {
      if (log_size_for_flush == port::kMaxUint64) {
        flush_memtable = false;
      } else if (log_size_for_flush > 0) {
        // If out standing log files are small, we skip the flush.
        s = db_->GetSortedWalFiles(live_wal_files);

        if (!s.ok()) {
          return s;
        }

        // Don't flush column families if total log size is smaller than
        // log_size_for_flush. We copy the log files instead.
        // We may be able to cover 2PC case too.
        uint64_t total_wal_size = 0;
        for (auto& wal : live_wal_files) {
          total_wal_size += wal->SizeFileBytes();
        }
        if (total_wal_size < log_size_for_flush) {
          flush_memtable = false;
        }
        live_wal_files.clear();
      }
    }

    // this will return live_files prefixed with "/"
    s = db_->GetLiveFiles(live_files, &manifest_file_size, flush_memtable);

    if (s.ok() && db_options.allow_2pc) {
      // If 2PC is enabled, we need to get minimum log number after the flush.
      // Need to refetch the live files to recapture the snapshot.
      if (!db_->GetIntProperty(DB::Properties::kMinLogNumberToKeep,
                               &min_log_num)) {
        return Status::InvalidArgument(
            "2PC enabled but cannot fine the min log number to keep.");
      }
      // We need to refetch live files with flush to handle this case:
      // A previous 000001.log contains the prepare record of transaction tnx1.
      // The current log file is 000002.log, and sequence_number points to this
      // file.
      // After calling GetLiveFiles(), 000003.log is created.
      // Then tnx1 is committed. The commit record is written to 000003.log.
      // Now we fetch min_log_num, which will be 3.
      // Then only 000002.log and 000003.log will be copied, and 000001.log will
      // be skipped. 000003.log contains commit message of tnx1, but we don't
      // have respective prepare record for it.
      // In order to avoid this situation, we need to force flush to make sure
      // all transactions committed before getting min_log_num will be flushed
      // to SST files.
      // We cannot get min_log_num before calling the GetLiveFiles() for the
      // first time, because if we do that, all the logs files will be included,
      // far more than needed.
      s = db_->GetLiveFiles(live_files, &manifest_file_size, flush_memtable);
    }

    TEST_SYNC_POINT("CheckpointImpl::CreateCheckpoint:SavedLiveFiles1");
    TEST_SYNC_POINT("CheckpointImpl::CreateCheckpoint:SavedLiveFiles2");
    db_->FlushWAL(false /* sync */);
  }
  // if we have more than one column family, we need to also get WAL files
  if (s.ok()) {
    s = db_->GetSortedWalFiles(live_wal_files);
  }
  if (!s.ok()) {
    return s;
  }

  size_t wal_size = live_wal_files.size();

  // copy/hard link live_files
  std::string manifest_fname, current_fname;
  for (size_t i = 0; s.ok() && i < live_files.size(); ++i) {
    uint64_t number;
    FileType type;
    bool ok = ParseFileName(live_files[i], &number, &type);
    if (!ok) {
      s = Status::Corruption("Can't parse file name. This is very bad");
      break;
    }
    // we should only get sst, options, manifest and current files here
    assert(type == kTableFile || type == kDescriptorFile ||
           type == kCurrentFile || type == kOptionsFile);
    assert(live_files[i].size() > 0 && live_files[i][0] == '/');
    if (type == kCurrentFile) {
      // We will craft the current file manually to ensure it's consistent with
      // the manifest number. This is necessary because current's file contents
      // can change during checkpoint creation.
      current_fname = live_files[i];
      continue;
    } else if (type == kDescriptorFile) {
      manifest_fname = live_files[i];
    }
    std::string src_fname = live_files[i];

    // rules:
    // * if it's kTableFile, then it's shared
    // * if it's kDescriptorFile, limit the size to manifest_file_size
    // * always copy if cross-device link
    if ((type == kTableFile) && same_fs) {
      s = link_file_cb(db_->GetName(), src_fname, type);
      if (s.IsNotSupported()) {
        same_fs = false;
        s = Status::OK();
      }
    }
    if ((type != kTableFile) || (!same_fs)) {
      s = copy_file_cb(db_->GetName(), src_fname,
                       (type == kDescriptorFile) ? manifest_file_size : 0,
                       type);
    }
  }
  if (s.ok() && !current_fname.empty() && !manifest_fname.empty()) {
    create_file_cb(current_fname, manifest_fname.substr(1) + "\n",
                   kCurrentFile);
  }
  ROCKS_LOG_INFO(db_options.info_log, "Number of log files %" ROCKSDB_PRIszt,
                 live_wal_files.size());

  // Link WAL files. Copy exact size of last one because it is the only one
  // that has changes after the last flush.
  for (size_t i = 0; s.ok() && i < wal_size; ++i) {
    if ((live_wal_files[i]->Type() == kAliveLogFile) &&
        (!flush_memtable ||
         live_wal_files[i]->StartSequence() >= *sequence_number ||
         live_wal_files[i]->LogNumber() >= min_log_num)) {
      if (i + 1 == wal_size) {
        s = copy_file_cb(db_options.wal_dir, live_wal_files[i]->PathName(),
                         live_wal_files[i]->SizeFileBytes(), kLogFile);
        break;
      }
      if (same_fs) {
        // we only care about live log files
        s = link_file_cb(db_options.wal_dir, live_wal_files[i]->PathName(),
                         kLogFile);
        if (s.IsNotSupported()) {
          same_fs = false;
          s = Status::OK();
        }
      }
      if (!same_fs) {
        s = copy_file_cb(db_options.wal_dir, live_wal_files[i]->PathName(), 0,
                         kLogFile);
      }
    }
  }

  return s;
}

struct LogReporter : public log::Reader::Reporter {
  Status* status = nullptr;

  void Corruption(size_t bytes, const Status& s) override {
    if (this->status && this->status->ok()) *this->status = s;
  }
};

static Status ModifyManifestFileLastSeq(Env* env, const DBOptions& db_options,
                                        const std::string& file_name,
                                        SequenceNumber last_seq) {
  Status s;
  EnvOptions env_options(db_options);
  std::string tmp_file = file_name + ".tmp";
  s = env->RenameFile(file_name, tmp_file);
  if (!s.ok()) {
    return s;
  }

  unique_ptr<SequentialFileReader> file_reader;
  {
    unique_ptr<SequentialFile> file;
    s = env->NewSequentialFile(tmp_file, &file, env_options);
    if (!s.ok()) {
      return s;
    }

    file_reader.reset(new SequentialFileReader(std::move(file)));
  }

  unique_ptr<log::Writer> file_writer;
  {
    unique_ptr<WritableFile> file;
    EnvOptions opt_env_opts = env->OptimizeForManifestWrite(env_options);
    s = env->NewWritableFile(file_name, &file, opt_env_opts);
    if (!s.ok()) {
      return s;
    }

    file->SetPreallocationBlockSize(db_options.manifest_preallocation_size);
    unique_ptr<WritableFileWriter> writer(
        new WritableFileWriter(std::move(file), opt_env_opts));
    file_writer.reset(new log::Writer(std::move(writer),
                                      0 /*log_number. log_number is useless when recycle_log_files is false.*/,
                                      false /*recycle_log_files*/,
                                      false /*manual_flush*/));
  }

  {
    LogReporter reporter;
    reporter.status = &s;
    log::Reader reader(db_options.info_log /*actually not used in Reader.*/,
                       std::move(file_reader), &reporter, true /*checksum*/,
                       0 /*initial_offset*/, 0 /*log_num. log_number is useless when recycle_log_files is false in writer.*/);
    Slice record;
    std::string scratch;
    while (reader.ReadRecord(&record, &scratch) && s.ok()) {
      VersionEdit edit;
      s = edit.DecodeFrom(record);
      if (!s.ok()) {
        break;
      }
      if (edit.HasLastSequence() && edit.GetLastSequence() > last_seq) {
        edit.SetLastSequence(last_seq);
      }
      std::string write_record;
      if (!edit.EncodeTo(&write_record)) {
        s = Status::Corruption("Unable to Encode VersionEdit");
        break;
      }
      s = file_writer->AddRecord(write_record);
      if (!s.ok()) {
        break;
      }
    }
  }

  if (s.ok()) {
    ImmutableDBOptions imop(db_options);
    s = SyncManifest(env, &imop, file_writer->file());
  }
  file_reader.reset();
  file_writer.reset();
  if (s.ok()) {
    env->DeleteFile(tmp_file);
  }
  return s;
}

Status CheckpointImpl::CreateCheckpointQuick(const std::string& checkpoint_dir,
                                             uint64_t* checkpoint_decree) {
  Status s;
  std::vector<std::string> live_files;
  uint64_t manifest_file_size = 0;
  SequenceNumber last_sequence = 0;
  uint64_t last_decree = 0;
  bool same_fs = true;

  s = db_->GetEnv()->FileExists(checkpoint_dir);
  if (s.ok()) {
    return Status::InvalidArgument("Directory exists");
  } else if (!s.IsNotFound()) {
    assert(s.IsIOError());
    return s;
  }

  s = db_->DisableFileDeletions();
  if (s.ok()) {
    // this will return live_files prefixed with "/"
    s = db_->GetLiveFilesQuick(live_files, &manifest_file_size, &last_sequence,
                               &last_decree);
  }
  if (!s.ok()) {
    db_->EnableFileDeletions(false);
    return s;
  }

  std::string full_private_path = checkpoint_dir + ".tmp";
  std::string manifest_file_path;
  uint64_t manifest_file_number = 0;

  // create snapshot directory
  s = db_->GetEnv()->CreateDir(full_private_path);

  // copy/hard link live_files
  for (size_t i = 0; s.ok() && i < live_files.size(); ++i) {
    uint64_t number;
    FileType type;
    bool ok = ParseFileName(live_files[i], &number, &type);
    if (!ok) {
      s = Status::Corruption("Can't parse file name. This is very bad");
      break;
    }
    // we should only get sst, manifest, current and options files here
    assert(type == kTableFile || type == kDescriptorFile ||
           type == kCurrentFile || type == kOptionsFile);
    assert(live_files[i].size() > 0 && live_files[i][0] == '/');
    const std::string& src_fname = live_files[i];

    // do not copy current file because it may have been updated to
    // point to a new manifest file, so we create it by ourselves
    if (type == kCurrentFile) {
      continue;
    }

    // rules:
    // * if it's kTableFile, then it's shared
    // * if it's kDescriptorFile, limit the size to manifest_file_size
    // * always copy if cross-device link
    if ((type == kTableFile) && same_fs) {
      Log(db_->GetOptions().info_log, "Hard Linking %s", src_fname.c_str());
      s = db_->GetEnv()->LinkFile(db_->GetName() + src_fname,
                                  full_private_path + src_fname);
      if (s.IsNotSupported()) {
        same_fs = false;
        s = Status::OK();
      }
    }
    if ((type != kTableFile) || (!same_fs)) {
      Log(db_->GetOptions().info_log, "Copying %s", src_fname.c_str());
      s = CopyFile(db_->GetEnv(), db_->GetName() + src_fname,
                   full_private_path + src_fname,
                   (type == kDescriptorFile) ? manifest_file_size : 0,
                   false /*use_fsync*/);
    }
    if (type == kDescriptorFile) {
      manifest_file_path = full_private_path + src_fname;
      manifest_file_number = number;
    }
  }

  // we copied all the files, enable file deletions
  db_->EnableFileDeletions(false);

  if (s.ok()) {
    // modify manifest file to set correct last_seq in VersionEdit, because
    // the last_seq recorded in manifest may be greater than the real value
    assert(!manifest_file_path.empty());
    s = ModifyManifestFileLastSeq(db_->GetEnv(), db_->GetOptions(),
                                  manifest_file_path, last_sequence);
  }
  if (s.ok()) {
    // make current file
    assert(manifest_file_number != 0);
    s = SetCurrentFile(db_->GetEnv(), full_private_path,
                       manifest_file_number, nullptr);
  }
  if (s.ok()) {
    // move tmp private backup to real snapshot directory
    s = db_->GetEnv()->RenameFile(full_private_path, checkpoint_dir);
  }
  if (s.ok()) {
    unique_ptr<Directory> checkpoint_directory;
    db_->GetEnv()->NewDirectory(checkpoint_dir, &checkpoint_directory);
    if (checkpoint_directory != nullptr) {
      s = checkpoint_directory->Fsync();
    }
  }

  if (!s.ok()) {
    // clean all the files we might have created
    Log(db_->GetOptions().info_log, "Snapshot failed -- %s",
        s.ToString().c_str());
    // we have to delete the dir and all its children
    std::vector<std::string> subchildren;
    db_->GetEnv()->GetChildren(full_private_path, &subchildren);
    for (auto& subchild : subchildren) {
      Status s1 = db_->GetEnv()->DeleteFile(full_private_path + subchild);
      if (s1.ok()) {
        Log(db_->GetOptions().info_log, "Deleted %s",
            (full_private_path + subchild).c_str());
      }
    }
    // finally delete the private dir
    Status s1 = db_->GetEnv()->DeleteDir(full_private_path);
    Log(db_->GetOptions().info_log, "Deleted dir %s -- %s",
        full_private_path.c_str(), s1.ToString().c_str());
    return s;
  }

  // here we know that we succeeded and installed the new snapshot
  Log(db_->GetOptions().info_log,
      "Snapshot DONE. All is good. seqno: %" PRIu64 ", decree: %" PRIu64 "",
      last_sequence, last_decree);

  if (checkpoint_decree != nullptr) {
    *checkpoint_decree = last_decree;
  }

  return s;
}

}  // namespace rocksdb

#endif  // ROCKSDB_LITE
