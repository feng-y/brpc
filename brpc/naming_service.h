// Baidu RPC - A framework to host and access services throughout Baidu.
// Copyright (c) 2014 Baidu.com, Inc. All Rights Reserved

// Author: The baidu-rpc authors (pbrpc@baidu.com)
// Date: Sun Aug 31 20:02:37 CST 2014

#ifndef BRPC_NAMING_SERVICE_H
#define BRPC_NAMING_SERVICE_H

#include <vector>                                   // std::vector
#include <string>                                   // std::string
#include <ostream>                                  // std::ostream
#include "base/endpoint.h"                          // base::EndPoint
#include "base/macros.h"                            // BAIDU_CONCAT
#include "brpc/describable.h"
#include "brpc/destroyable.h"
#include "brpc/extension.h"                    // Extension<T>


namespace brpc {

// Representing a server inside a NamingService.
struct ServerNode {
    ServerNode() {}
    ServerNode(base::ip_t ip, int port, const std::string& tag2)
        : addr(ip, port), tag(tag2) {}
    ServerNode(const base::EndPoint& pt, const std::string& tag2)
        : addr(pt), tag(tag2) {}
    ServerNode(base::ip_t ip, int port) : addr(ip, port) {}
    explicit ServerNode(const base::EndPoint& pt) : addr(pt) {}

    base::EndPoint addr;
    std::string tag;
};

// Continuing actions to added/removed servers.
// NOTE: You don't have to implement this class.
class NamingServiceActions {
public:
    virtual ~NamingServiceActions() {}
    virtual void AddServers(const std::vector<ServerNode>& servers) = 0;
    virtual void RemoveServers(const std::vector<ServerNode>& servers) = 0;
    virtual void ResetServers(const std::vector<ServerNode>& servers) = 0;
};

// Mapping a name to ServerNodes.
class NamingService : public Describable, public Destroyable {
public:    
    // Implement this method to get servers associated with `service_name'
    // in periodic or event-driven manner, call methods of `actions' to
    // tell RPC system about server changes. This method will be run in
    // a dedicated bthread without access from other threads, thus the
    // implementation does NOT need to be thread-safe.
    // Return 0 on success, error code otherwise.
    virtual int RunNamingService(const char* service_name,
                                 NamingServiceActions* actions) = 0;

    // If this method returns true, RunNamingService will be called without
    // a dedicated bthread. As the name implies, this is suitable for static
    // and simple impl, saving the cost of creating a bthread. However most
    // impl of RunNamingService never quit, thread is a must to prevent the
    // method from blocking the caller.
    virtual bool RunNamingServiceReturnsQuickly() { return false; }

    // Create/destroy an instance.
    // Caller is responsible for Destroy() the instance after usage.
    virtual NamingService* New() const = 0;

protected:
    virtual ~NamingService() {}
};

inline Extension<const NamingService>* NamingServiceExtension() {
    return Extension<const NamingService>::instance();
}

inline bool operator<(const ServerNode& n1, const ServerNode& n2)
{ return n1.addr != n2.addr ? (n1.addr < n2.addr) : (n1.tag < n2.tag); }
inline bool operator==(const ServerNode& n1, const ServerNode& n2)
{ return n1.addr == n2.addr && n1.tag == n2.tag; }
inline bool operator!=(const ServerNode& n1, const ServerNode& n2)
{ return !(n1 == n2); }
inline std::ostream& operator<<(std::ostream& os, const ServerNode& n) {
    os << n.addr;
    if (!n.tag.empty()) {
        os << "(tag=" << n.tag << ')';
    }
    return os;
}

} // namespace brpc


#endif  // BRPC_NAMING_SERVICE_H
