// Baidu RPC - A framework to host and access services throughout Baidu.
// Copyright (c) 2014 Baidu.com, Inc. All Rights Reserved

// Author: The baidu-rpc authors (pbrpc@baidu.com)
// Date: Mon Sep 29 13:33:36 CST 2014

#include "base/macros.h"
#include "base/fast_rand.h"
#include "brpc/socket.h"
#include "brpc/policy/randomized_load_balancer.h"


namespace brpc {
namespace policy {

const uint32_t prime_offset[] = {
#include "bthread/offset_inl.list"
};

inline uint32_t GenRandomStride() {
    return prime_offset[base::fast_rand_less_than(ARRAY_SIZE(prime_offset))];
}

bool RandomizedLoadBalancer::Add(Servers& bg, const ServerId& id) {
    if (bg.server_list.capacity() < 128) {
        bg.server_list.reserve(128);
    }
    std::map<ServerId, size_t>::iterator it = bg.server_map.find(id);
    if (it != bg.server_map.end()) {
        return false;
    }
    bg.server_map[id] = bg.server_list.size();
    bg.server_list.push_back(id);
    return true;
}

bool RandomizedLoadBalancer::Remove(Servers& bg, const ServerId& id) {
    std::map<ServerId, size_t>::iterator it = bg.server_map.find(id);
    if (it != bg.server_map.end()) {
        size_t index = it->second;
        bg.server_list[index] = bg.server_list.back();
        bg.server_map[bg.server_list[index]] = index;
        bg.server_list.pop_back();
        bg.server_map.erase(it);
        return true;
    }
    return false;
}

size_t RandomizedLoadBalancer::BatchAdd(
    Servers& bg, const std::vector<ServerId>& servers) {
    size_t count = 0;
    for (size_t i = 0; i < servers.size(); ++i) {
        count += !!Add(bg, servers[i]);
    }
    return count;
}

size_t RandomizedLoadBalancer::BatchRemove(
    Servers& bg, const std::vector<ServerId>& servers) {
    size_t count = 0;
    for (size_t i = 0; i < servers.size(); ++i) {
        count += !!Remove(bg, servers[i]);
    }
    return count;
}

bool RandomizedLoadBalancer::AddServer(const ServerId& id) {
    return _db_servers.Modify(Add, id);
}

bool RandomizedLoadBalancer::RemoveServer(const ServerId& id) {
    return _db_servers.Modify(Remove, id);
}

size_t RandomizedLoadBalancer::AddServersInBatch(
    const std::vector<ServerId>& servers) {
    const size_t n = _db_servers.Modify(BatchAdd, servers);
    LOG_IF(ERROR, n != servers.size())
        << "Fail to AddServersInBatch, expected " << servers.size()
        << " actually " << n;
    return n;
}

size_t RandomizedLoadBalancer::RemoveServersInBatch(
    const std::vector<ServerId>& servers) {
    const size_t n = _db_servers.Modify(BatchRemove, servers);
    LOG_IF(ERROR, n != servers.size())
        << "Fail to RemoveServersInBatch, expected " << servers.size()
        << " actually " << n;
    return n;
}

int RandomizedLoadBalancer::SelectServer(const SelectIn& in, SelectOut* out) {
    base::DoublyBufferedData<Servers>::ScopedPtr s;
    if (_db_servers.Read(&s) != 0) {
        return ENOMEM;
    }
    size_t n = s->server_list.size();
    if (n == 0) {
        return ENODATA;
    }
    
    uint32_t stride = 0;
    size_t offset = base::fast_rand_less_than(n);
    for (size_t i = 0; i < n; ++i) {
        const SocketId id = s->server_list[offset].id;
        if (((i + 1) == n  // always take last chance
             || !ExcludedServers::IsExcluded(in.excluded, id))
            && Socket::Address(id, out->ptr) == 0
            && !(*out->ptr)->IsLogOff()) {
            // We found an available server
            return 0;
        }
        if (stride == 0) {
            stride = GenRandomStride();
        }
        // If `Address' failed, use `offset+stride' to retry so that
        // this failed server won't be visited again inside for
        offset = (offset + stride) % n;
    }
    // After we traversed the whole server list, there is still no
    // available server
    return EHOSTDOWN;
}

RandomizedLoadBalancer* RandomizedLoadBalancer::New() const {
    return new (std::nothrow) RandomizedLoadBalancer;
}

void RandomizedLoadBalancer::Destroy() {
    delete this;
}

void RandomizedLoadBalancer::Describe(
    std::ostream &os, const DescribeOptions& options) {
    if (!options.verbose) {
        os << "random";
        return;
    }
    os << "Randomized{";
    base::DoublyBufferedData<Servers>::ScopedPtr s;
    if (_db_servers.Read(&s) != 0) {
        os << "fail to read _db_servers";
    } else {
        os << "n=" << s->server_list.size() << ':';
        for (size_t i = 0; i < s->server_list.size(); ++i) {
            os << ' ' << s->server_list[i];
        }
    }
    os << '}';
}

}  // namespace policy
} // namespace brpc

