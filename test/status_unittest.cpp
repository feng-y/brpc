// Copyright (c) 2010-2014 Baidu.com, Inc. All Rights Reserved
//
// Author: Ge,Jun (gejun@baidu.com)
// Date: 2010-12-04 11:59

#include <errno.h>
#include <gtest/gtest.h>
#include "base/status.h"

namespace {
class StatusTest : public ::testing::Test{
protected:
    StatusTest(){
    };
    virtual ~StatusTest(){};
    virtual void SetUp() {
        srand(time(0));
    };
    virtual void TearDown() {
    };
};

TEST_F(StatusTest, success_status) {
    std::ostringstream oss;

    base::Status st;
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(0, st.error_code());
    ASSERT_STREQ("OK", st.error_cstr());
    ASSERT_EQ("OK", st.error_str());
    oss.str("");
    oss << st;
    ASSERT_EQ("OK", oss.str());

    base::Status st2(0, "blahblah");
    ASSERT_TRUE(st2.ok());
    ASSERT_EQ(0, st2.error_code());
    ASSERT_STREQ("OK", st2.error_cstr());
    ASSERT_EQ("OK", st2.error_str());
    oss.str("");
    oss << st2;
    ASSERT_EQ("OK", oss.str());

    base::Status st3 = base::Status::OK();
    ASSERT_TRUE(st3.ok());
    ASSERT_EQ(0, st3.error_code());
    ASSERT_STREQ("OK", st3.error_cstr());
    ASSERT_EQ("OK", st3.error_str());
    oss.str("");
    oss << st3;
    ASSERT_EQ("OK", oss.str());
}

#define NO_MEMORY_STR "No memory"
#define NO_CPU_STR "No CPU"

TEST_F(StatusTest, failed_status) {
    std::ostringstream oss;

    base::Status st1(ENOMEM, NO_MEMORY_STR);
    ASSERT_FALSE(st1.ok());
    ASSERT_EQ(ENOMEM, st1.error_code());
    ASSERT_STREQ(NO_MEMORY_STR, st1.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR, st1.error_str());
    oss.str("");
    oss << st1;
    ASSERT_EQ(NO_MEMORY_STR, oss.str());

    base::Status st2(EINVAL, "%s%s", NO_MEMORY_STR, NO_CPU_STR);
    ASSERT_FALSE(st2.ok());
    ASSERT_EQ(EINVAL, st2.error_code());
    ASSERT_STREQ(NO_MEMORY_STR NO_CPU_STR, st2.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR NO_CPU_STR, st2.error_str());
    oss.str("");
    oss << st2;
    ASSERT_EQ(NO_MEMORY_STR NO_CPU_STR, oss.str());

    base::Status st3(ENOMEM, NO_MEMORY_STR);
    ASSERT_FALSE(st3.ok());
    ASSERT_EQ(ENOMEM, st3.error_code());
    ASSERT_STREQ(NO_MEMORY_STR, st3.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR, st3.error_str());
    oss.str("");
    oss << st3;
    ASSERT_EQ(NO_MEMORY_STR, oss.str());

    base::Status st4(EINVAL, "%s%s", NO_MEMORY_STR, NO_CPU_STR);
    ASSERT_FALSE(st4.ok());
    ASSERT_EQ(EINVAL, st4.error_code());
    ASSERT_STREQ(NO_MEMORY_STR NO_CPU_STR, st4.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR NO_CPU_STR, st4.error_str());
    oss.str("");
    oss << st4;
    ASSERT_EQ(NO_MEMORY_STR NO_CPU_STR, oss.str());

    base::Status st5(EINVAL, "Blah");
    ASSERT_FALSE(st5.ok());
    ASSERT_EQ(EINVAL, st5.error_code());
    ASSERT_STREQ("Blah", st5.error_cstr());
    ASSERT_EQ(std::string("Blah"), st5.error_str());
    oss.str("");
    oss << st5;
    ASSERT_EQ(std::string("Blah"), oss.str());
}

#define VERYLONGERROR                           \
    "verylongverylongverylongverylongverylongverylongverylongverylong"  \
    "verylongverylongverylongverylongverylongverylongverylongverylong"  \
    "verylongverylongverylongverylongverylongverylongverylongverylong"  \
    "verylongverylongverylongverylongverylongverylongverylongverylong"  \
    "verylongverylongverylongverylongverylongverylongverylongverylong"  \
    "verylongverylongverylongverylongverylongverylongverylongverylong"  \
    "verylongverylongverylongverylongverylongverylongverylongverylong"  \
    "verylongverylongverylongverylongverylongverylongverylongverylong"  \
    "verylongverylongverylongverylongverylongverylongverylongverylong"  \
    "verylongverylongverylongverylongverylongverylongverylongverylong"  \
    "verylongverylongverylongverylongverylongverylongverylongverylong"  \
    " error"

TEST_F(StatusTest, reset) {
    std::ostringstream oss;

    base::Status st1(ENOMEM, NO_MEMORY_STR);
    ASSERT_FALSE(st1.ok());
    ASSERT_EQ(ENOMEM, st1.error_code());
    ASSERT_STREQ(NO_MEMORY_STR, st1.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR, st1.error_str());
    oss.str("");
    oss << st1;
    ASSERT_EQ(NO_MEMORY_STR, oss.str());

    ASSERT_EQ(0, st1.set_error(EINVAL, "%s%s", NO_MEMORY_STR, NO_CPU_STR));
    ASSERT_FALSE(st1.ok());
    ASSERT_EQ(EINVAL, st1.error_code());
    ASSERT_STREQ(NO_MEMORY_STR NO_CPU_STR, st1.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR NO_CPU_STR, st1.error_str());
    oss.str("");
    oss << st1;
    ASSERT_EQ(NO_MEMORY_STR NO_CPU_STR, oss.str());

    ASSERT_EQ(0, st1.set_error(ENOMEM, NO_MEMORY_STR));
    ASSERT_FALSE(st1.ok());
    ASSERT_EQ(ENOMEM, st1.error_code());
    ASSERT_STREQ(NO_MEMORY_STR, st1.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR, st1.error_str());
    oss.str("");
    oss << st1;
    ASSERT_EQ(NO_MEMORY_STR, oss.str());

    st1.reset();
    ASSERT_TRUE(st1.ok());
    ASSERT_EQ(0, st1.error_code());
    ASSERT_STREQ("OK", st1.error_cstr());
    ASSERT_EQ("OK", st1.error_str());
    oss.str("");
    oss << st1;
    ASSERT_EQ("OK", oss.str());

    ASSERT_EQ(0, st1.set_error(ENOMEM, "%s", VERYLONGERROR));
    ASSERT_FALSE(st1.ok());
    ASSERT_EQ(ENOMEM, st1.error_code());
    ASSERT_STREQ(VERYLONGERROR, st1.error_cstr());
    ASSERT_EQ(VERYLONGERROR, st1.error_str());
    oss.str("");
    oss << st1;
    ASSERT_EQ(VERYLONGERROR, oss.str());
}

TEST_F(StatusTest, copy) {
    std::ostringstream oss;

    base::Status st1(ENOMEM, NO_MEMORY_STR);
    ASSERT_FALSE(st1.ok());
    ASSERT_EQ(ENOMEM, st1.error_code());
    ASSERT_STREQ(NO_MEMORY_STR, st1.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR, st1.error_str());
    oss.str("");
    oss << st1;
    ASSERT_EQ(NO_MEMORY_STR, oss.str());

    base::Status st2;
    ASSERT_TRUE(st2.ok());
    ASSERT_EQ(0, st2.error_code());
    ASSERT_STREQ("OK", st2.error_cstr());
    ASSERT_EQ("OK", st2.error_str());
    oss.str("");
    oss << st2;
    ASSERT_EQ("OK", oss.str());

    st2 = st1;
    ASSERT_FALSE(st2.ok());
    ASSERT_EQ(ENOMEM, st2.error_code());
    ASSERT_STREQ(NO_MEMORY_STR, st2.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR, st2.error_str());
    oss.str("");
    oss << st2;
    ASSERT_EQ(NO_MEMORY_STR, oss.str());
    
    ASSERT_EQ(0, st1.set_error(EINVAL, "%s%s", NO_MEMORY_STR, NO_CPU_STR));
    ASSERT_FALSE(st1.ok());
    ASSERT_EQ(EINVAL, st1.error_code());
    ASSERT_STREQ(NO_MEMORY_STR NO_CPU_STR, st1.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR NO_CPU_STR, st1.error_str());
    oss.str("");
    oss << st1;
    ASSERT_EQ(NO_MEMORY_STR NO_CPU_STR, oss.str());

    ASSERT_FALSE(st2.ok());
    ASSERT_EQ(ENOMEM, st2.error_code());
    ASSERT_STREQ(NO_MEMORY_STR, st2.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR, st2.error_str());
    oss.str("");
    oss << st2;
    ASSERT_EQ(NO_MEMORY_STR, oss.str());

    st2 = st1;
    ASSERT_FALSE(st2.ok());
    ASSERT_EQ(EINVAL, st2.error_code());
    ASSERT_STREQ(NO_MEMORY_STR NO_CPU_STR, st2.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR NO_CPU_STR, st2.error_str());
    oss.str("");
    oss << st2;
    ASSERT_EQ(NO_MEMORY_STR NO_CPU_STR, oss.str());

    ASSERT_EQ(0, st1.set_error(ENOMEM, NO_MEMORY_STR));
    ASSERT_FALSE(st1.ok());
    ASSERT_EQ(ENOMEM, st1.error_code());
    ASSERT_STREQ(NO_MEMORY_STR, st1.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR, st1.error_str());
    oss.str("");
    oss << st1;
    ASSERT_EQ(NO_MEMORY_STR, oss.str());

    // assign shorter to longer, no memory allocation.
    st2 = st1;
    ASSERT_FALSE(st2.ok());
    ASSERT_EQ(ENOMEM, st2.error_code());
    ASSERT_STREQ(NO_MEMORY_STR, st2.error_cstr());
    ASSERT_EQ(NO_MEMORY_STR, st2.error_str());
    oss.str("");
    oss << st2;
    ASSERT_EQ(NO_MEMORY_STR, oss.str());
}

TEST_F(StatusTest, message_has_zero) {
    std::ostringstream oss;
    char str[32] = "hello world";
    base::StringPiece slice(str);
    ASSERT_EQ(11UL, slice.as_string().size());
    str[5] = '\0';
    ASSERT_EQ(11UL, slice.as_string().size());
    base::Status st1(ENOMEM, slice);
    ASSERT_FALSE(st1.ok());
    ASSERT_EQ(ENOMEM, st1.error_code());
    ASSERT_STREQ("hello", st1.error_cstr());
    oss.str("");
    oss << st1;
    ASSERT_EQ(st1.error_str(), oss.str());
}
}
