// Copyright (c) 2010-2014 Baidu.com, Inc. All Rights Reserved
//
// Author: Ge,Jun (gejun@baidu.com)
// Date: 2010-12-04 11:59

#include <gtest/gtest.h>
#include "base/errno.h"
#include "base/endpoint.h"
#include "base/logging.h"

namespace {

TEST(EndPointTest, comparisons) {
    base::EndPoint p1(base::int2ip(1234), 5678);
    base::EndPoint p2 = p1;
    ASSERT_TRUE(p1 == p2 && !(p1 != p2));
    ASSERT_TRUE(p1 <= p2 && p1 >= p2 && !(p1 < p2 || p1 > p2));
    ++p2.port;
    ASSERT_TRUE(p1 != p2 && !(p1 == p2));
    ASSERT_TRUE(p1 < p2 && p2 > p1 && !(p2 <= p1 || p1 >= p2));
    --p2.port;
    p2.ip = base::int2ip(base::ip2int(p2.ip)-1);
    ASSERT_TRUE(p1 != p2 && !(p1 == p2));
    ASSERT_TRUE(p1 > p2 && p2 < p1 && !(p1 <= p2 || p2 >= p1));
}

TEST(EndPointTest, ip_t) {
    LOG(INFO) << "INET_ADDRSTRLEN = " << INET_ADDRSTRLEN;
    
    base::ip_t ip0;
    ASSERT_EQ(0, base::str2ip("1.1.1.1", &ip0));
    ASSERT_STREQ("1.1.1.1", base::ip2str(ip0).c_str());
    ASSERT_EQ(-1, base::str2ip("301.1.1.1", &ip0));
    ASSERT_EQ(-1, base::str2ip("1.-1.1.1", &ip0));
    ASSERT_EQ(-1, base::str2ip("1.1.-101.1", &ip0));
    ASSERT_STREQ("1.0.0.0", base::ip2str(base::int2ip(1)).c_str());

    base::ip_t ip1, ip2, ip3;
    ASSERT_EQ(0, base::str2ip("192.168.0.1", &ip1));
    ASSERT_EQ(0, base::str2ip("192.168.0.2", &ip2));
    ip3 = ip1;
    ASSERT_LT(ip1, ip2);
    ASSERT_LE(ip1, ip2);
    ASSERT_GT(ip2, ip1);
    ASSERT_GE(ip2, ip1);
    ASSERT_TRUE(ip1 != ip2);
    ASSERT_FALSE(ip1 == ip2);
    ASSERT_TRUE(ip1 == ip3);
    ASSERT_FALSE(ip1 != ip3);
}

TEST(EndPointTest, show_local_info) {
    LOG(INFO) << "my_ip is " << base::my_ip() << std::endl
              << "my_ip_cstr is " << base::my_ip_cstr() << std::endl
              << "my_hostname is " << base::my_hostname();
}

TEST(EndPointTest, endpoint) {
    base::EndPoint p1;
    ASSERT_EQ(base::IP_ANY, p1.ip);
    ASSERT_EQ(0, p1.port);
    
    base::EndPoint p2(base::IP_NONE, -1);
    ASSERT_EQ(base::IP_NONE, p2.ip);
    ASSERT_EQ(-1, p2.port);

    base::EndPoint p3;
    ASSERT_EQ(-1, base::str2endpoint(" 127.0.0.1:-1", &p3));
    ASSERT_EQ(-1, base::str2endpoint(" 127.0.0.1:65536", &p3));
    ASSERT_EQ(0, base::str2endpoint(" 127.0.0.1:65535", &p3));
    ASSERT_EQ(0, base::str2endpoint(" 127.0.0.1:0", &p3));

    base::EndPoint p4;
    ASSERT_EQ(0, base::str2endpoint(" 127.0.0.1: 289 ", &p4));
    ASSERT_STREQ("127.0.0.1", base::ip2str(p4.ip).c_str());
    ASSERT_EQ(289, p4.port);
    
    base::EndPoint p5;
    ASSERT_EQ(-1, hostname2endpoint("localhost:-1", &p5));
    ASSERT_EQ(-1, hostname2endpoint("localhost:65536", &p5));
    ASSERT_EQ(0, hostname2endpoint("localhost:65535", &p5)) << berror();
    ASSERT_EQ(0, hostname2endpoint("localhost:0", &p5));

    base::EndPoint p6;
    ASSERT_EQ(0, hostname2endpoint("tc-cm-et21.tc: 289 ", &p6));
    ASSERT_STREQ("10.23.249.73", base::ip2str(p6.ip).c_str());
    ASSERT_EQ(289, p6.port);
}

TEST(EndPointTest, hash_table) {
    base::hash_map<base::EndPoint, int> m;
    base::EndPoint ep1(base::IP_ANY, 123);
    base::EndPoint ep2(base::IP_ANY, 456);
    ++m[ep1];
    ASSERT_TRUE(m.find(ep1) != m.end());
    ASSERT_EQ(1, m.find(ep1)->second);
    ASSERT_EQ(1u, m.size());

    ++m[ep1];
    ASSERT_TRUE(m.find(ep1) != m.end());
    ASSERT_EQ(2, m.find(ep1)->second);
    ASSERT_EQ(1u, m.size());

    ++m[ep2];
    ASSERT_TRUE(m.find(ep2) != m.end());
    ASSERT_EQ(1, m.find(ep2)->second);
    ASSERT_EQ(2u, m.size());
}

}
