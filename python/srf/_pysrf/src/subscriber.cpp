/**
 * SPDX-FileCopyrightText: Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "pysrf/subscriber.hpp"

#include "pysrf/operators.hpp"
#include "pysrf/types.hpp"

#include <glog/logging.h>
#include <pybind11/cast.h>
#include <pybind11/eval.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>  // IWYU pragma: keep
#include <pybind11/pytypes.h>
#include <rxcpp/rx.hpp>

#include <array>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace srf::pysrf {

namespace py = pybind11;
using namespace py::literals;

void SubscriberProxy::on_next(PyObjectSubscriber* self, py::object&& value)
{
    // Check to see if we are subscribed before sending the value
    if (self->is_subscribed())
    {
        self->on_next(std::move(value));
    }
};

void SubscriberProxy::on_error(PyObjectSubscriber* self, py::object&& value)
{
    try
    {
        //  Raise the exception locally to convert to C++ exception
        auto locals = py::dict("__on_error_exception"_a = std::move(value));
        py::exec("raise __on_error_exception", py::globals(), locals);
    } catch (py::error_already_set)
    {
        py::gil_scoped_release nogil;

        self->on_error(std::current_exception());
    }
};

bool SubscriberProxy::is_subscribed(PyObjectSubscriber* self)
{
    // No GIL here
    return self->is_subscribed();
}

}  // namespace srf::pysrf
