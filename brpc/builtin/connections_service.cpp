// Baidu RPC - A framework to host and access services throughout Baidu.
// Copyright (c) 2014 Baidu.com, Inc. All Rights Reserved
//
// Author: The baidu-rpc authors (pbrpc@baidu.com)
// Date: Wed Apr  8 18:18:06 2015

#include <ostream>
#include <iomanip>
#include <netinet/tcp.h>
#include <gflags/gflags.h>
#include "brpc/closure_guard.h"        // ClosureGuard
#include "brpc/controller.h"           // Controller
#include "brpc/socket_map.h"           // SocketMapList
#include "brpc/acceptor.h"             // Acceptor
#include "brpc/server.h"
#include "brpc/nshead_service.h"
#include "brpc/builtin/common.h"
#include "brpc/builtin/connections_service.h"


namespace brpc {

DEFINE_bool(show_hostname_instead_of_ip, false,
            "/connections shows hostname instead of ip");
BRPC_VALIDATE_GFLAG(show_hostname_instead_of_ip, PassValidate);

DEFINE_int32(max_shown_connections, 1024,
             "Print stats of at most so many connections (soft limit)");

// NOTE: The returned string must be 3-char wide in text mode.
inline const char* SSLStateToYesNo(SSLState s, bool use_html) {
    switch (s) {
    case SSL_UNKNOWN:
        return (use_html ? "-" : " - ");
    case SSL_CONNECTING:
    case SSL_CONNECTED:
        return "Yes";
    case SSL_OFF:
        return "No ";
    }
    return "Bad";
}

struct NameOfPoint {
    explicit NameOfPoint(const base::EndPoint& pt_) : pt(pt_) {}
    base::EndPoint pt;
};

std::ostream& operator<<(std::ostream& os, const NameOfPoint& nop) {
    char buf[128];
    if (FLAGS_show_hostname_instead_of_ip &&
        base::endpoint2hostname(nop.pt, buf, sizeof(buf)) == 0) {
        return os << buf;
    } else {
        return os << nop.pt;
    }
}

inline bool EndsWith(const std::string& str, const char* suffix) {
    const size_t len = strlen(suffix);
    return str.size() >= len &&
        memcmp(str.data() + str.size() - len, suffix, len) == 0;
}

static std::string BriefName(const std::string& cname) {
    size_t pos = cname.rfind(':');
    if (pos == std::string::npos) {
        pos = 0;
    } else {
        ++pos;
    }
    size_t end = cname.size();
    if (EndsWith(cname, "ServiceAdaptor")) {
        end = cname.size() - 14;
    } else if (EndsWith(cname, "Adaptor")) {
        end = cname.size() - 7;
    } else if (EndsWith(cname, "Protocol")) {
        end = cname.size() - 8;
    } else if (EndsWith(cname, "Service")) {
        end = cname.size() - 7;
    }
    std::string tmp;
    tmp.reserve(end - pos + 4);
    for (; pos < end; ++pos) {
        if (::isupper(cname[pos])) {
            if (!tmp.empty()) {
                tmp.push_back('_');
            }
            tmp.push_back(::tolower(cname[pos]));
        } else {
            tmp.push_back(cname[pos]);
        }
    }
    return tmp;
}

void ConnectionsService::PrintConnections(
    std::ostream& os, const std::vector<SocketId>& conns,
    bool use_html, const Server* server, bool need_local) const {
    if (conns.empty()) {
        return;
    }
    if (use_html) {
        os << "<table class=\"gridtable sortable\" border=\"1\"><tr>"
            "<th>CreatedTime</th>"
            "<th>RemoteSide</th>";
        if (need_local) {
            os << "<th>Local</th>";
        }
        os << "<th>SSL</th>"
            "<th>Protocol</th>"
            "<th>fd</th>"
            "<th>InBytes/s</th>"
            "<th>In/s</th>"
            "<th>InBytes/m</th>"
            "<th>In/m</th>"
            "<th>OutBytes/s</th>"
            "<th>Out/s</th>"
            "<th>OutBytes/m</th>"
            "<th>Out/m</th>"
            "<th>Rtt/Var(ms)</th>"
            "<th>SocketId</th>"
            "</tr>\n";
    } else {
        os << "CreatedTime               |RemoteSide         |";
        if (need_local) {
            os << "Local|";
        }
        os << "SSL|Protocol |fd   |"
            "InBytes/s|In/s  |InBytes/m |In/m    |"
            "OutBytes/s|Out/s |OutBytes/m|Out/m   |"
            "Rtt/Var(ms)|SocketId\n";
    }

    const char* const bar = (use_html ? "</td><td>" : "|");
    SocketStat stat;
    std::string nshead_service_name; // shared in following for iterations.
    std::vector<SocketId> first_id;
    for (size_t i = 0; i < conns.size(); ++i) {
        const SocketId socket_id = conns[i];
        SocketUniquePtr ptr;
        bool failed = false;
        if (Socket::Address(socket_id, &ptr) != 0) {
            int ret = Socket::AddressFailedAsWell(socket_id, &ptr);
            if (ret < 0) {
                continue;
            } else if (ret > 0) {
                if (ptr->_health_check_interval_s <= 0) {
                    // Sockets without HC will soon be destroyed
                    continue;
                }
                failed = true;
            }
        }            

        if (use_html) {
            os << "<tr><td>";
        }
        if (failed) {
            os << min_width("Broken", 26) << bar
               << min_width(NameOfPoint(ptr->remote_side()), 19) << bar;
            if (need_local) {
                os << min_width(ptr->local_side().port, 5) << bar;
            }
            os << min_width("-", 3) << bar
               << min_width("-", 9) << bar
               << min_width("-", 5) << bar
               << min_width("-", 9) << bar
               << min_width("-", 6) << bar
               << min_width("-", 10) << bar
               << min_width("-", 8) << bar
               << min_width("-", 10) << bar
               << min_width("-", 6) << bar
               << min_width("-", 10) << bar
               << min_width("-", 8) << bar
               << min_width("-", 11) << bar;
        } else {
            // Get name of the protocol. In principle we can dynamic_cast the
            // socket user to InputMessenger but I'm not sure if that's a bit
            // slow (because we have many connections here).
            int pref_index = ptr->preferred_index();
            SocketUniquePtr first_sub;
            if (pref_index < 0) {
                // Check preferred_index of any pooled sockets.
                ptr->ListPooledSockets(&first_id, 1);
                if (!first_id.empty()) {
                    if (Socket::Address(first_id[0], &first_sub) == 0) {
                        pref_index = first_sub->preferred_index();
                    }
                }
            }
            const char* pref_prot = "-";
            if (ptr->user() == server->_am) {
                pref_prot = server->_am->NameOfProtocol(pref_index);
                // Special treatment for nshead services. Notice that
                // pref_index is comparable to ProtocolType after r31951
                if (pref_index == (int)PROTOCOL_NSHEAD &&
                    server->options().nshead_service != NULL) {
                    if (nshead_service_name.empty()) {
                        nshead_service_name = BriefName(base::class_name_str(
                                *server->options().nshead_service));
                    }
                    pref_prot = nshead_service_name.c_str();
                }
            } else if (ptr->CreatedByConnect()) {
                pref_prot = get_client_side_messenger()->NameOfProtocol(pref_index);
            }
            if (strcmp(pref_prot, "unknown") == 0) {
                // Show unknown protocol as - to be consistent with other columns.
                pref_prot = "-";
            }
            ptr->GetStat(&stat);
            PrintRealDateTime(os, ptr->_reset_fd_real_us);
            int rttfd = ptr->fd();
            if (rttfd < 0 && first_sub != NULL) {
                rttfd = first_sub->fd();
            }
            // get rtt, this is linux-specific
            struct tcp_info ti;
            socklen_t len = sizeof(ti);
            char rtt_display[32];
            if (0 == getsockopt(rttfd, SOL_TCP, TCP_INFO, &ti, &len)) {
                snprintf(rtt_display, sizeof(rtt_display), "%.1f/%.1f",
                         ti.tcpi_rtt / 1000.0, ti.tcpi_rttvar / 1000.0);
            } else {
                strcpy(rtt_display, "-");
            }
            os << bar << min_width(NameOfPoint(ptr->remote_side()), 19) << bar;
            if (need_local) {
                if (ptr->local_side().port > 0) {
                    os << min_width(ptr->local_side().port, 5) << bar;
                } else {
                    os << min_width((first_sub ? "*" : "-"), 5) << bar;
                }
            }
            os << SSLStateToYesNo(ptr->ssl_state(), use_html) << bar
               << min_width(pref_prot, 9) << bar;
            if (ptr->fd() >= 0) {
                os << min_width(ptr->fd(), 5) << bar;
            } else {
                os << min_width((first_sub ? "*" : "-"), 5) << bar;
            }
            os << min_width(stat.in_size_s, 9) << bar
               << min_width(stat.in_num_messages_s, 6) << bar
               << min_width(stat.in_size_m, 10) << bar
               << min_width(stat.in_num_messages_m, 8) << bar
               << min_width(stat.out_size_s, 10) << bar
               << min_width(stat.out_num_messages_s, 6) << bar
               << min_width(stat.out_size_m, 10) << bar
               << min_width(stat.out_num_messages_m, 8) << bar
               << min_width(rtt_display, 11) << bar;
        }

        if (use_html) {
            os << "<a href=\"/sockets/" << socket_id << "\">" << socket_id
               << "</a>";
        } else {
            os << socket_id;
        }
        if (use_html) {
            os << "</td></tr>";
        }
        os << '\n';
    }
    if (use_html) {
        os << "</table>\n";
    }
}

void ConnectionsService::default_method(
    ::google::protobuf::RpcController* cntl_base,
    const ::brpc::ConnectionsRequest*,
    ::brpc::ConnectionsResponse*,
    ::google::protobuf::Closure* done) {
    ClosureGuard done_guard(done);
    Controller *cntl = static_cast<Controller*>(cntl_base);
    const Server* server = cntl->server();
    Acceptor* am = server->_am;
    Acceptor* internal_am = server->_internal_am;
    base::IOBufBuilder os;
    const bool use_html = UseHTML(cntl->http_request());
    cntl->http_response().set_content_type(
        use_html ? "text/html" : "text/plain");

    if (use_html) {
        os << "<!DOCTYPE html><html><head>\n"
           << gridtable_style()
           << "<script src=\"/js/sorttable\"></script>\n"
           << "<script language=\"javascript\" type=\"text/javascript\" src=\"/js/jquery_min\"></script>\n"
           << TabsHead()
           << "</head><body>";
        server->PrintTabsBody(os, "connections");
    }

    size_t max_shown = (size_t)FLAGS_max_shown_connections;
    if (cntl->http_request().uri().GetQuery("givemeall")) {
        max_shown = std::numeric_limits<size_t>::max();
    }
    bool has_uncopied = false;
    std::vector<SocketId> conns;
    // NOTE: not accurate count.
    size_t num_conns = am->ConnectionCount();
    am->ListConnections(&conns, max_shown);
    if (conns.size() == max_shown && num_conns > conns.size()) {
        // OK to have false positive
        has_uncopied = true;
    }
    if (internal_am) {
        size_t num_conns2 = internal_am->ConnectionCount();
        std::vector<SocketId> internal_conns;
        // Connections to internal_port are generally small, thus
        // -max_shown_connections is treated as a soft limit
        internal_am->ListConnections(&internal_conns, max_shown);
        if (internal_conns.size() == max_shown &&
            num_conns2 > internal_conns.size()) {
            // OK to have false positive
            has_uncopied = true;
        }
        conns.insert(conns.end(), internal_conns.begin(), internal_conns.end());
    }
    os << "server_socket_count: " << num_conns << '\n';
    PrintConnections(os, conns, use_html, server, false/*need_local*/);
    if (has_uncopied) {
        // Notice that we don't put the link of givemeall directly because
        // people seeing the link are very likely to click it which may be
        // slow and should generally be avoided.
        os << "(Stop printing more connections... check out all connections"
            " by appending \"?givemeall\" to the url of current page)"
           << (use_html ? "<br>\n" : "\n");
    }

    SocketMapList(&conns);
    os << (use_html ? "<br>\n" : "\n")
       << "channel_socket_count: " << conns.size() << '\n';
    PrintConnections(os, conns, use_html, server, true/*need_local*/);

    if (use_html) {
        os << "</body></html>\n";
    }
    
    os.move_to(cntl->response_attachment());
    cntl->set_response_compress_type(COMPRESS_TYPE_GZIP);
}

void ConnectionsService::GetTabInfo(TabInfoList* info_list) const {
    TabInfo* info = info_list->add();
    info->path = "/connections";
    info->tab_name = "connections";
}

} // namespace brpc

