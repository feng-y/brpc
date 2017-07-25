// Copyright (c) 2015 Baidu.com, Inc. All Rights Reserved
// Author: Ge,Jun (gejun@baidu.com)
// Date: Sun Aug  9 12:26:03 CST 2015

#ifndef  BVAR_GFLAG_H
#define  BVAR_GFLAG_H

#include <string>                       // std::string
#include "bvar/variable.h"

namespace bvar {

// Expose important gflags as bvar so that they're monitored.
class GFlag : public Variable {
public:
    GFlag(const base::StringPiece& gflag_name);
    
    GFlag(const base::StringPiece& prefix,
          const base::StringPiece& gflag_name);
    
    // Calling hide() in dtor manually is a MUST required by Variable.
    ~GFlag() { hide(); }

    // Implement Variable::describe() and Variable::get_value().
    void describe(std::ostream& os, bool quote_string) const;

#ifdef BAIDU_INTERNAL
    void get_value(boost::any* value) const;
#endif

    // Get value of the gflag.
    // We don't bother making the return type generic. This function
    // is just for consistency with other classes.
    std::string get_value() const;

    // Set the gflag with a new value.
    // Returns true on success.
    bool set_value(const char* value);

    // name of the gflag.
    const std::string& gflag_name() const {
        return _gflag_name.empty() ? name() : _gflag_name;
    }
    
private:
    std::string _gflag_name;
};

}  // namespace bvar

#endif  //BVAR_GFLAG_H
