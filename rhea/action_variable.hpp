//---------------------------------------------------------------------------
/// \file   action_variable.hpp
/// \brief  A variable that calls a function whenever it changes
//
// Copyright 2015, nocte@hippie.nu            Released under the MIT License.
//---------------------------------------------------------------------------
#pragma once

#include "float_variable.hpp"
#include <functional>

namespace rhea
{

/** A variable that calls a function whenever it changes.
 */
class action_variable : public float_variable
{
public:
    action_variable(std::function<void(double)> callback, double value)
        : float_variable{value}
        , callback_{callback}
    {
    }

    virtual ~action_variable() {}

    virtual void set_value(double new_value)
    {
        value_ = new_value;
        callback_(new_value);
    }

protected:
    std::function<void(double)> callback_;
};

} // namespace rhea
