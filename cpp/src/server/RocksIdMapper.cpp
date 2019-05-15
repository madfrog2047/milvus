/*******************************************************************************
 * Copyright 上海赜睿信息科技有限公司(Zilliz) - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited.
 * Proprietary and confidential.
 ******************************************************************************/

#include "RocksIdMapper.h"
#include "ServerConfig.h"
#include "utils/Log.h"
#include "utils/CommonUtil.h"

#include "rocksdb/slice.h"
#include "rocksdb/options.h"

#include <exception>

namespace zilliz {
namespace vecwise {
namespace server {

RocksIdMapper::RocksIdMapper()
: db_(nullptr) {
    OpenDb();
}

RocksIdMapper::~RocksIdMapper() {
    CloseDb();
}

void RocksIdMapper::OpenDb() {
    if(db_) {
        return;
    }

    ConfigNode& config = ServerConfig::GetInstance().GetConfig(CONFIG_DB);
    std::string db_path = config.GetValue(CONFIG_DB_PATH);
    db_path += "/id_mapping";
    CommonUtil::CreateDirectory(db_path);

    rocksdb::Options options;
    // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
    options.IncreaseParallelism();
    options.OptimizeLevelStyleCompaction();
    // create the DB if it's not already present
    options.create_if_missing = true;
    options.max_open_files = config.GetInt32Value(CONFIG_DB_IDMAPPER_MAX_FILE, 512);

    //load column families
    std::vector<std::string> column_names;
    rocksdb::Status s = rocksdb::DB::ListColumnFamilies(options, db_path, &column_names);
    if (!s.ok()) {
        SERVER_LOG_ERROR << "ID mapper failed to initialize:" << s.ToString();
    }

    if(column_names.empty()) {
        column_names.push_back("default");
    }
    SERVER_LOG_INFO << "ID mapper has " << std::to_string(column_names.size()) << " groups";

    std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
    for(auto& column_name : column_names) {
        rocksdb::ColumnFamilyDescriptor desc;
        desc.name = column_name;
        column_families.emplace_back(desc);
    }

    // open DB
    std::vector<rocksdb::ColumnFamilyHandle*> column_handles;
    s = rocksdb::DB::Open(options, db_path, column_families, &column_handles, &db_);
    if(!s.ok()) {
        SERVER_LOG_ERROR << "ID mapper failed to initialize:" << s.ToString();
        db_ = nullptr;
    }

    column_handles_.clear();
    for(auto handler : column_handles) {
        column_handles_.insert(std::make_pair(handler->GetName(), handler));
    }
}

void RocksIdMapper::CloseDb() {
    for(auto& iter : column_handles_) {
        delete iter.second;
    }
    column_handles_.clear();

    if(db_) {
        db_->Close();
        delete db_;
    }
}

ServerError RocksIdMapper::Put(const std::string& nid, const std::string& sid, const std::string& group) {
    if(db_ == nullptr) {
        return SERVER_NULL_POINTER;
    }

    rocksdb::Slice key(nid);
    rocksdb::Slice value(sid);
    if(group.empty()) {//to default group
        rocksdb::Status s = db_->Put(rocksdb::WriteOptions(), key, value);
        if (!s.ok()) {
            SERVER_LOG_ERROR << "ID mapper failed to put:" << s.ToString();
            return SERVER_UNEXPECTED_ERROR;
        }
    } else {
        rocksdb::ColumnFamilyHandle *cfh = nullptr;
        if(column_handles_.count(group) == 0) {
            try {//add group
                rocksdb::Status s = db_->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), group, &cfh);
                if (!s.ok()) {
                    SERVER_LOG_ERROR << "ID mapper failed to create group:" << s.ToString();
                } else {
                    column_handles_.insert(std::make_pair(group, cfh));
                }
            } catch(std::exception& ex) {
                std::cout << ex.what() << std::endl;
            }
        } else {
            cfh = column_handles_[group];
        }

        rocksdb::Status s = db_->Put(rocksdb::WriteOptions(), cfh, key, value);
        if (!s.ok()) {
            SERVER_LOG_ERROR << "ID mapper failed to put:" << s.ToString();
            return SERVER_UNEXPECTED_ERROR;
        }
    }

    return SERVER_SUCCESS;
}

ServerError RocksIdMapper::Put(const std::vector<std::string>& nid, const std::vector<std::string>& sid, const std::string& group) {
    if(nid.size() != sid.size()) {
        return SERVER_INVALID_ARGUMENT;
    }

    ServerError err = SERVER_SUCCESS;
    for(size_t i = 0; i < nid.size(); i++) {
        err = Put(nid[i], sid[i], group);
        if(err != SERVER_SUCCESS) {
            return err;
        }
    }

    return err;
}

ServerError RocksIdMapper::Get(const std::string& nid, std::string& sid, const std::string& group) const {
    sid = "";
    if(db_ == nullptr) {
        return SERVER_NULL_POINTER;
    }

    rocksdb::ColumnFamilyHandle *cfh = nullptr;
    if(column_handles_.count(group) != 0) {
        cfh = column_handles_.at(group);
    }

    rocksdb::Slice key(nid);
    rocksdb::Status s;
    if(cfh){
        s = db_->Get(rocksdb::ReadOptions(), cfh, key, &sid);
    } else {
        s = db_->Get(rocksdb::ReadOptions(), key, &sid);
    }

    if(!s.ok()) {
        SERVER_LOG_ERROR << "ID mapper failed to get:" << s.ToString();
        return SERVER_UNEXPECTED_ERROR;
    }

    return SERVER_SUCCESS;
}

ServerError RocksIdMapper::Get(const std::vector<std::string>& nid, std::vector<std::string>& sid, const std::string& group) const {
    sid.clear();

    ServerError err = SERVER_SUCCESS;
    for(size_t i = 0; i < nid.size(); i++) {
        std::string str_id;
        ServerError temp_err = Get(nid[i], str_id, group);
        if(temp_err != SERVER_SUCCESS) {
            sid.push_back("");
            SERVER_LOG_ERROR << "ID mapper failed to get id: " << nid[i];
            err = temp_err;
            continue;
        }

        sid.push_back(str_id);
    }

    return err;
}

ServerError RocksIdMapper::Delete(const std::string& nid, const std::string& group) {
    if(db_ == nullptr) {
        return SERVER_NULL_POINTER;
    }

    rocksdb::ColumnFamilyHandle *cfh = nullptr;
    if(column_handles_.count(group) != 0) {
        cfh = column_handles_.at(group);
    }

    rocksdb::Slice key(nid);
    rocksdb::Status s;
    if(cfh){
        s = db_->Delete(rocksdb::WriteOptions(), cfh, key);
    } else {
        s = db_->Delete(rocksdb::WriteOptions(), key);
    }
    if(!s.ok()) {
        SERVER_LOG_ERROR << "ID mapper failed to delete:" << s.ToString();
        return SERVER_UNEXPECTED_ERROR;
    }

    return SERVER_SUCCESS;
}

ServerError RocksIdMapper::DeleteGroup(const std::string& group) {
    if(db_ == nullptr) {
        return SERVER_NULL_POINTER;
    }

    rocksdb::ColumnFamilyHandle *cfh = nullptr;
    if(column_handles_.count(group) != 0) {
        cfh = column_handles_.at(group);
    }

    if(cfh) {
        db_->DropColumnFamily(cfh);
        db_->DestroyColumnFamilyHandle(cfh);
        column_handles_.erase(group);
    }

    return SERVER_SUCCESS;
}

}
}
}