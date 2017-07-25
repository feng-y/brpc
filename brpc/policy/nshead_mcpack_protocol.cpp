// Baidu RPC - A framework to host and access services throughout Baidu.
// Copyright (c) 2016 Baidu.com, Inc. All Rights Reserved

// Author: The baidu-rpc authors (pbrpc@baidu.com)
// Date: Tue Apr 19 18:17:26 CST 2016

#include <google/protobuf/descriptor.h>         // MethodDescriptor
#include <google/protobuf/message.h>            // Message
#include <gflags/gflags.h>

#include "base/time.h"
#include "base/iobuf.h"                        // base::IOBuf

#include "brpc/controller.h"               // Controller
#include "brpc/socket.h"                   // Socket
#include "brpc/server.h"                   // Server
#include "brpc/details/server_private_accessor.h"
#include "brpc/span.h"
#include "brpc/errno.pb.h"                 // EREQUEST, ERESPONSE
#include "brpc/details/controller_private_accessor.h"
#include "brpc/policy/most_common_message.h"
#include "brpc/policy/nshead_mcpack_protocol.h"
#include "mcpack2pb/mcpack2pb.h"


namespace brpc {
namespace policy {

void NsheadMcpackAdaptor::ParseNsheadMeta(
    const Server& svr, const NsheadMessage& /*request*/, Controller* cntl,
    NsheadMeta* out_meta) const {
    google::protobuf::Service* service = svr.first_service();
    if (!service) {
        cntl->SetFailed(ENOSERVICE, "No first_service in this server");
        return;
    }
    const google::protobuf::ServiceDescriptor* sd = service->GetDescriptor();
    if (sd->method_count() == 0) {
        cntl->SetFailed(ENOMETHOD, "No method in service=%s",
                        sd->full_name().c_str());
        return;
    }
    const google::protobuf::MethodDescriptor* method = sd->method(0);
    out_meta->set_full_method_name(method->full_name());
}

void NsheadMcpackAdaptor::ParseRequestFromIOBuf(
    const NsheadMeta&, const NsheadMessage& raw_req,
    Controller* cntl, google::protobuf::Message* pb_req) const {
    const std::string& msg_name = pb_req->GetDescriptor()->full_name();
    mcpack2pb::MessageHandler handler = mcpack2pb::find_message_handler(msg_name);
    if (!handler.parse_from_iobuf(pb_req, raw_req.body)) {
        cntl->SetFailed(EREQUEST, "Fail to parse request message, "
                        "request_size=%" PRIu64, raw_req.body.length());
        return;
    }
}

void NsheadMcpackAdaptor::SerializeResponseToIOBuf(
    const NsheadMeta&, Controller* cntl,
    const google::protobuf::Message* pb_res, NsheadMessage* raw_res) const {
    if (cntl->Failed()) {
        cntl->CloseConnection("Close connection due to previous error");
        return;
    }
    CompressType type = cntl->response_compress_type();
    if (type != COMPRESS_TYPE_NONE) {
        LOG(WARNING) << "nshead_mcpack protocol doesn't support compression";
        type = COMPRESS_TYPE_NONE;
    }
    
    if (pb_res == NULL) {
        cntl->CloseConnection("response was not created yet");
        return;
    }

    const std::string& msg_name = pb_res->GetDescriptor()->full_name();
    mcpack2pb::MessageHandler handler = mcpack2pb::find_message_handler(msg_name);
    if (!handler.serialize_to_iobuf(*pb_res, &raw_res->body,
                                   ::mcpack2pb::FORMAT_MCPACK_V2)) {
        cntl->CloseConnection("Fail to serialize %s", msg_name.c_str());
        return;
    }
}

void ProcessNsheadMcpackResponse(InputMessageBase* msg_base) {
    const int64_t start_parse_us = base::cpuwide_time_us();
    DestroyingPtr<MostCommonMessage> msg(static_cast<MostCommonMessage*>(msg_base));
    const Socket* socket = msg->socket();
    
    // Fetch correlation id that we saved before in `PackNsheadMcpackRequest'
    const bthread_id_t cid = { static_cast<uint64_t>(socket->correlation_id()) };
    Controller* cntl = NULL;
    const int rc = bthread_id_lock(cid, (void**)&cntl);
    if (rc != 0) {
        LOG_IF(FATAL, rc != EINVAL) << "Fail to lock correlation_id="
                                    << cid.value << ": " << berror(rc);
        return;
    }
    
    ControllerPrivateAccessor accessor(cntl);
    Span* span = accessor.span();
    if (span) {
        span->set_base_real_us(msg->base_real_us());
        span->set_received_us(msg->received_us());
        span->set_response_size(msg->meta.size() + msg->payload.size());
        span->set_start_parse_us(start_parse_us);
    }
    const int saved_error = cntl->ErrorCode();
    google::protobuf::Message* res = cntl->response();
    if (res == NULL) {
        // silently ignore response.
        return;
    }
    const std::string& msg_name = res->GetDescriptor()->full_name();
    mcpack2pb::MessageHandler handler = mcpack2pb::find_message_handler(msg_name);
    if (!handler.parse_from_iobuf(res, msg->payload)) {
        return cntl->CloseConnection("Fail to parse response message");
    }
    // Unlocks correlation_id inside. Revert controller's
    // error code if it version check of `cid' fails
    msg.reset();  // optional, just release resourse ASAP
    accessor.OnResponse(cid, saved_error);
} 

void SerializeNsheadMcpackRequest(base::IOBuf* buf, Controller* cntl,
                          const google::protobuf::Message* pb_req) {
    CompressType type = cntl->request_compress_type();
    if (type != COMPRESS_TYPE_NONE) {
        cntl->SetFailed(EREQUEST,
                        "nshead_mcpack protocol doesn't support compression");
        return;
    }
    const std::string& msg_name = pb_req->GetDescriptor()->full_name();
    mcpack2pb::MessageHandler handler = mcpack2pb::find_message_handler(msg_name);
    if (!handler.serialize_to_iobuf(*pb_req, buf, ::mcpack2pb::FORMAT_MCPACK_V2)) {
        cntl->SetFailed(EREQUEST, "Fail to serialize %s", msg_name.c_str());
        return;
    }
}

void PackNsheadMcpackRequest(base::IOBuf* buf,
                             SocketMessage**,
                             uint64_t correlation_id,
                             const google::protobuf::MethodDescriptor*,
                             Controller* controller,
                             const base::IOBuf& request,
                             const Authenticator* /*not supported*/) {
    ControllerPrivateAccessor accessor(controller);
    if (accessor.connection_type() == CONNECTION_TYPE_SINGLE) {
        return controller->SetFailed(
            EINVAL, "nshead_mcpack can't work with CONNECTION_TYPE_SINGLE");
    }
    // Store `correlation_id' into Socket since nshead_mcpack protocol
    // doesn't contain this field
    accessor.set_socket_correlation_id(correlation_id);
        
    nshead_t nshead;
    memset(&nshead, 0, sizeof(nshead_t));
    nshead.log_id = controller->log_id();
    nshead.magic_num = NSHEAD_MAGICNUM;
    nshead.body_len = request.size();
    buf->append(&nshead, sizeof(nshead));

    // Span* span = accessor.span();
    // if (span) {
    //     request_meta->set_trace_id(span->trace_id());
    //     request_meta->set_span_id(span->span_id());
    //     request_meta->set_parent_span_id(span->parent_span_id());
    // }
    buf->append(request);
}

}  // namespace policy
} // namespace brpc

