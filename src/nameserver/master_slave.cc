// Copyright (c) 2016, Baidu.com, Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#include <sys/stat.h>
#include <common/string_util.h>
#include <common/logging.h>
#include <common/timer.h>
#include <gflags/gflags.h>

#include "nameserver/master_slave.h"
#include "proto/status_code.pb.h"
#include "rpc/rpc_client.h"

DECLARE_string(nameserver_nodes);
DECLARE_int32(node_index);
DECLARE_string(master_slave_role);
DECLARE_int64(log_gc_interval);

namespace baidu {
namespace bfs {

MasterSlaveImpl::MasterSlaveImpl() : exiting_(false), master_only_(false),
                                     cond_(&mu_), log_done_(&mu_), current_idx_(-1),
                                     applied_idx_(-1), sync_idx_(-1), snapshot_idx_(-1),
                                     snapshot_step_(-1) {
    std::vector<std::string> nodes;
    common::SplitString(FLAGS_nameserver_nodes, ",", &nodes);
    std::string this_server = nodes[FLAGS_node_index];
    std::string another_server;
    if (FLAGS_node_index == 0) {
        another_server = nodes[1];
    } else if (FLAGS_node_index == 1) {
        another_server = nodes[0];
    } else {
        LOG(FATAL, "\033[32m[Sync]\033[0m Nameserver does not belong to this cluster");
    }
    master_addr_ = FLAGS_master_slave_role == "master" ? this_server: another_server;
    slave_addr_ = FLAGS_master_slave_role == "slave" ? this_server: another_server;
    is_leader_ = FLAGS_master_slave_role == "master";
    if (IsLeader()) {
        LOG(INFO, "\033[32m[Sync]\033[0m I am Leader");
    } else {
        LOG(INFO, "\033[32m[Sync]\033[0m I am Slave");
    }
    thread_pool_ = new common::ThreadPool(10);
    DBOption option;
    LogDB::Open("./logdb", option, &logdb_);
    if (logdb_ == NULL) {
        LOG(FATAL, "init logdb failed");
    }
}

void MasterSlaveImpl::Init(boost::function<void (const std::string& log, int64_t)> callback,
                           boost::function<void (int64_t, std::string*, bool*)> scan_func) {
    log_callback_ = callback;
    scan_func_ = scan_func;
    if (logdb_->GetLargestIdx(&current_idx_) == kReadError) {
        LOG(FATAL, "\033[32m[Sync]\033[0m  Read current_idx_ failed");
    }
    if (logdb_->ReadMarker("applied_idx", &applied_idx_) == kReadError) {
        LOG(FATAL, "\033[32m[Sync]\033[0m  ReadMarker applied_idx_ failed");
    }
    if (logdb_->ReadMarker("sync_idx", &sync_idx_) == kReadError) {
        LOG(FATAL, "\033[32m[Sync]\033[0m  ReadMarker sync_idx_ failed");
    }
    StatusCode s = logdb_->ReadMarker("snapshot_idx", &snapshot_idx_);
    if (s == kReadError) {
        LOG(FATAL, "\033[32m[Sync]\033[0m  ReadMarker snapshot_idx_ failed");
    } else if (s == kNsNotFound && IsLeader()) {
        snapshot_idx_ = 0;
    }
    LOG(INFO, "\033[32m[Sync]\033[0m set current_idx_ = %ld, applied_idx_ = %ld, sync_idx_ = %ld, snapshot_idx_ = %ld",
            current_idx_, applied_idx_, sync_idx_, snapshot_idx_);
    assert(applied_idx_ <= current_idx_ && sync_idx_ <= current_idx_);
    while (applied_idx_ < current_idx_) {
        std::string entry;
        StatusCode ret = logdb_->Read(applied_idx_ + 1, &entry);
        if (ret != kOK) {
            LOG(FATAL, "\033[32m[Sync]\033[0m read logdb failed index %ld %s",
                    applied_idx_ + 1, StatusCode_Name(ret).c_str());
        }
        if (!entry.empty()) {
            log_callback_(entry, -1);
        }
        applied_idx_++;
    }

    rpc_client_ = new RpcClient();
    rpc_client_->GetStub(slave_addr_, &slave_stub_);
    if (IsLeader()) {
        worker_.Start(boost::bind(&MasterSlaveImpl::BackgroundLog, this));
    }
    LogStatus();
}

bool MasterSlaveImpl::IsLeader(std::string* leader_addr) {
    return is_leader_;
}

////// Master //////
bool MasterSlaveImpl::Log(const std::string& entry, int timeout_ms) {
    if (!IsLeader()) {
        return true;
    }
    mu_.Lock();
    if (logdb_->Write(current_idx_ + 1, entry) != kOK) {
        LOG(FATAL, "\033[32m[Sync]\033[0m write logdb failed index %ld", current_idx_ + 1);
    }
    current_idx_++;
    cond_.Signal();
    mu_.Unlock();
    // slave is way behind, do no wait
    if (master_only_ && sync_idx_ < current_idx_ - 1) {
        LOG(WARNING, "\033[32m[Sync]\033[0m Sync in maset-only mode, do not wait");
        applied_idx_ = current_idx_;
        return true;
    }

    int64_t start_point = common::timer::get_micros();
    int64_t stop_point = start_point + timeout_ms * 1000;
    while (sync_idx_ != current_idx_ && common::timer::get_micros() < stop_point) {
        int wait_time = (stop_point - common::timer::get_micros()) / 1000;
        MutexLock lock(&mu_);
        if (log_done_.TimeWait(wait_time)) {
            if (sync_idx_ != current_idx_) {
                continue;
            }
            if (master_only_) {
                LOG(INFO, "\033[32m[Sync]\033[0m leaves master-only mode");
                master_only_ = false;
            }
            LOG(INFO, "\033[32m[Sync]\033[0m sync log takes %ld ms",
                    (common::timer::get_micros() - start_point) / 1000);
            return true;
        } else {
            break;
        }
    }
    // log replicate time out
    LOG(WARNING, "\033[32m[Sync]\033[0m Sync log timeout, Sync is in master-only mode");
    master_only_ = true;
    return true;
}

void MasterSlaveImpl::Log(const std::string& entry, boost::function<void (int64_t)> callback) {
    if (!IsLeader()) {
        return;
    }
    MutexLock lock(&mu_);
    StatusCode s = logdb_->Write(current_idx_ + 1, entry);
    if (s != kOK) {
        if (s != kWriteError) {
            LOG(INFO, "\033[32m[Sync]\033[0m write logdb failed index %ld reason %s",
                current_idx_, StatusCode_Name(s).c_str());
        } else {
            LOG(FATAL, "\033[32m[Sync]\033[0m write logdb failed index %ld ", current_idx_ + 1);
        }
    }
    log_callback_(entry, current_idx_ + 1);
    current_idx_++;
    if (master_only_ && sync_idx_ < current_idx_ - 1) { // slave is behind, do not wait
        callbacks_.insert(std::make_pair(current_idx_, callback));
        thread_pool_->AddTask(boost::bind(&MasterSlaveImpl::ProcessCallbck,this,
                                            current_idx_, true));
    } else {
        callbacks_.insert(std::make_pair(current_idx_, callback));
        LOG(DEBUG, "\033[32m[Sync]\033[0m insert callback index = %d", current_idx_);
        thread_pool_->DelayTask(10000, boost::bind(&MasterSlaveImpl::ProcessCallbck,
                                                   this, current_idx_, true));
        cond_.Signal();
    }
    return;
}

void MasterSlaveImpl::SwitchToLeader() {
    if (IsLeader()) {
        return;
    }
    sync_idx_ = -1;
    std::string old_master_addr = master_addr_;
    master_addr_ = slave_addr_;
    slave_addr_ = old_master_addr;
    rpc_client_->GetStub(slave_addr_, &slave_stub_);
    worker_.Start(boost::bind(&MasterSlaveImpl::BackgroundLog, this));
    is_leader_ = true;
    master_only_ = true;
    LOG(INFO, "\033[32m[Sync]\033[0m node switch to leader");
}

///    Slave    ///
void MasterSlaveImpl::AppendLog(::google::protobuf::RpcController* controller,
                                const master_slave::AppendLogRequest* request,
                                master_slave::AppendLogResponse* response,
                                ::google::protobuf::Closure* done) {
    if (IsLeader()) { // already switched to leader, does not accept new append entries
        response->set_success(false);
        done->Run();
        return;
    }
    // expect snapshot id to be the same
    if (request->snapshot_idx() != snapshot_idx_) {
        response->set_snapshot_idx(snapshot_idx_);
        response->set_success(false);
        done->Run();
        return;
    }
    // expect index to be current_idx_ + 1
    if (request->index() > current_idx_ + 1) {
        response->set_index(current_idx_ + 1);
        response->set_success(false);
        done->Run();
        return;
    } else if (request->index() <= current_idx_) {
        LOG(INFO, "\033[32m[Sync]\033[0m out-date log request %ld, current_idx_ %ld",
            request->index(), current_idx_);
        response->set_index(current_idx_ + 1);
        response->set_success(false);
        done->Run();
        return;
    }
    mu_.Lock();
    if (logdb_->Write(current_idx_ + 1, request->log_data()) != kOK) {
        LOG(FATAL, "\033[32m[Sync]\033[0m Write logdb_ failed current_idx_ = %ld ", current_idx_ + 1);
    }
    current_idx_++;
    mu_.Unlock();
    log_callback_(request->log_data(), -1);
    applied_idx_ = current_idx_;
    response->set_success(true);
    done->Run();
}

///    Slave    ///
void MasterSlaveImpl::WriteSnapshot(::google::protobuf::RpcController* controller,
                                    const master_slave::WriteSnapshotReqeust* request,
                                    master_slave::WriteSnapshotResponse* response,
                                    ::google::protobuf::Closure* done) {
    if (IsLeader()) {
        LOG(WARNING, "\033[32m[Sync]\033[0m Leader should not receive WriteSnapshot request");
        done->Run();
        return;
    }
    int64_t idx = request->idx();
    int64_t step = request->step();
    if (step == 0) {
        snapshot_idx_ = idx;
        snapshot_step_ = 0;
        LOG(INFO, "\033[32m[Sync]\033[0m Start writing snapshot idx = %ld", idx);
    }
    if (idx != snapshot_idx_ || step != snapshot_step_) {
        response->set_success(false);
        response->set_idx(snapshot_idx_);
        response->set_step(snapshot_step_);
        LOG(INFO, "\033[32m[Sync]\033[0m WriteSnapshot rejected, req.idx = %ld req.step = %ld idx = %ld step = %ld",
                request->idx(), request->step(), snapshot_idx_, snapshot_step_);
        done->Run();
        return;
    }
    log_callback_(request->log_data(), -1);
    snapshot_step_ = step;
    if (request->done()) {
        mu_.Lock();
        if (!logdb_->WriteMarker("snapshot_idx", snapshot_idx_)) {
            LOG(FATAL, "\033[32m[Sync]\033[0m Update snapshot_idx_ failed %ld", snapshot_idx_);
        }
        LOG(INFO, "\033[32m[Sync]\033[0m WriteSnapshot done %ld", snapshot_idx_);
    }
    response->set_success(true);
    done->Run();
}

void MasterSlaveImpl::BackgroundLog() {
    while (true) {
        MutexLock lock(&mu_);
        while (!exiting_ && sync_idx_ == current_idx_) {
            LOG(DEBUG, "\033[32m[Sync]\033[0m BackgroundLog waiting...");
            cond_.Wait();
        }
        if (exiting_) {
            return;
        }
        LOG(DEBUG, "\033[32m[Sync]\033[0m BackgroundLog logging...");
        mu_.Unlock();
        ReplicateLog();
        mu_.Lock();
    }
}

void MasterSlaveImpl::ReplicateLog() {
    bool need_snapshot = false;
    while (sync_idx_ < current_idx_) {
        mu_.Lock();
        if (sync_idx_ == current_idx_) {
            mu_.Unlock();
            break;
        }
        LOG(DEBUG, "\033[32m[Sync]\033[0m ReplicateLog sync_idx_ = %d, current_idx_ = %d",
                sync_idx_, current_idx_);
        mu_.Unlock();
        std::string entry;
        StatusCode s = logdb_->Read(sync_idx_ + 1, &entry);
        if (need_snapshot || s == kNsNotFound) {
            LOG(INFO, "\033[32m[Sync]\033[0m Slave is too old, write snapshot to slave sync_idx_ = %ld", sync_idx_);
            if (ReplicateSnapshot()) {
                need_snapshot = false;
                LOG(INFO, "\033[32m[Sync]\033[0m WriteSnapshot %ld done", snapshot_idx_);
            } else {
                continue;
            }
        } else if (s != kOK & s != kNsNotFound) {
            LOG(FATAL, "\033[32m[Sync]\033[0m Read logdb failed sync_idx_ = %ld", sync_idx_);
        }
        master_slave::AppendLogRequest request;
        master_slave::AppendLogResponse response;
        request.set_log_data(entry);
        request.set_index(sync_idx_ + 1);
        request.set_snapshot_idx(snapshot_idx_);
        while (!rpc_client_->SendRequest(slave_stub_, &master_slave::MasterSlave_Stub::AppendLog,
                &request, &response, 15, 1)) {
            LOG(WARNING, "\033[32m[Sync]\033[0m Replicate log failed index = %ld current_idx_ = %ld snapshot_idx_ = %ld",
                sync_idx_ + 1, current_idx_, snapshot_idx_);
            sleep(5);
        }
        if (!response.success()) { // log mismatch
            MutexLock lock(&mu_);
            sync_idx_ = response.index() - 1;
            if (response.snapshot_idx() > snapshot_idx_) {
                LOG(WARNING, "Slave is ahead of master");
                sleep(5);
            } else if (response.snapshot_idx() < snapshot_idx_) {
                need_snapshot = true;
            }
            LOG(INFO, "\033[32m[Sync]\033[0m set sync_idx_ to %d need_snapshot = %d", sync_idx_, need_snapshot);
            continue;
        }
        thread_pool_->AddTask(boost::bind(&MasterSlaveImpl::ProcessCallbck, this, sync_idx_ + 1, false));
        mu_.Lock();
        sync_idx_++;
        LOG(DEBUG, "\033[32m[Sync]\033[0m Replicate log done. sync_idx_ = %d, current_idx_ = %d",
                sync_idx_ , current_idx_);
        mu_.Unlock();
    }
    applied_idx_ = current_idx_;
    log_done_.Signal();
}

bool MasterSlaveImpl::ReplicateSnapshot() {
    std::string log;
    bool done = false;
    int64_t step = 0;
    scan_func_(snapshot_idx_, NULL, &done);
    ++snapshot_idx_;
    mu_.Lock();
    if (logdb_->WriteMarker("snapshot_idx", snapshot_idx_) != kOK) {
        LOG(FATAL, "\033[32m[Sync]\033[0m Update snapshot_idx failed, idx = %ld", snapshot_idx_);
    }
    mu_.Unlock();
    while (!done) {
        scan_func_(snapshot_idx_, &log, &done);
        master_slave::WriteSnapshotReqeust request;
        master_slave::WriteSnapshotResponse response;
        request.set_idx(snapshot_idx_);
        request.set_step(step);
        request.set_done(done);
        request.set_log_data(log);
        while (!rpc_client_->SendRequest(slave_stub_, &master_slave::MasterSlave_Stub::WriteSnapshot,
                &request, &response, 15, 1)) {
            LOG(WARNING, "\033[32m[Sync]\033[0m ReplicateSnapshot failed idx = %d, step = %d",
                snapshot_idx_, step);
            sleep(5);
        }
        if (!response.success()) {
            LOG(WARNING, "\033[32m[Sync]\033[0m ReplicateSnapshot success = false, %ld %ld %ld %ld",
                    snapshot_idx_, step, response.idx(), response.step());
            return false;
        }
        ++step;
    }
    scan_func_(snapshot_idx_, NULL, &done);
    return true;
}

void MasterSlaveImpl::ProcessCallbck(int64_t index, bool timeout_check) {
    boost::function<void (int64_t)> callback;
    MutexLock lock(&mu_);
    std::map<int64_t, boost::function<void (int64_t)> >::iterator it = callbacks_.find(index);
    if (it != callbacks_.end()) {
        callback = it->second;
        LOG(DEBUG, "\033[32m[Sync]\033[0m calling callback %d", it->first);
        callbacks_.erase(it);
        mu_.Unlock();
        callback(index);
        mu_.Lock();
        if (index > applied_idx_) {
            applied_idx_ = index;
        }
        if (timeout_check) {
            if (!master_only_) {
                LOG(WARNING, "\033[32m[Sync]\033[0m ReplicateLog sync_idx_ = %d timeout, enter master-only mode",
                    index);
            }
            master_only_ = true;
            return;
        }
    }
    if (master_only_ && index == current_idx_) {
        LOG(INFO, "\033[32m[Sync]\033[0m leaves master-only mode");
        master_only_ = false;
    }
}

void MasterSlaveImpl::ClearLog() {
    logdb_->DeleteUpTo(sync_idx_);
    thread_pool_->DelayTask(FLAGS_log_gc_interval * 1000, boost::bind(&MasterSlaveImpl::ClearLog, this));
}

void MasterSlaveImpl::LogStatus() {
    LOG(INFO, "\033[32m[Sync]\033[0m sync_idx_ = %d, current_idx_ = %d, applied_idx_ = %d, callbacks_ size = %d snapshot_idx_ = %ld",
        sync_idx_, current_idx_, applied_idx_, callbacks_.size(), snapshot_idx_);
    mu_.Lock();
    StatusCode ret_a = logdb_->WriteMarker("applied_idx", applied_idx_);
    StatusCode ret_s = logdb_->WriteMarker("sync_idx", sync_idx_);
    mu_.Unlock();
    if ((ret_a != kOK) || (ret_s != kOK)) {
        LOG(WARNING, "\033[32m[Sync]\033[0m WriteMarker failed applied_idx_ = %ld sync_idx_ = %ld ",
                applied_idx_, sync_idx_);
    }
    thread_pool_->DelayTask(5000, boost::bind(&MasterSlaveImpl::LogStatus, this));
}

} // namespace bfs
} // namespace baidu
