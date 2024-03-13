/*
 * SPDX-FileCopyrightText: Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#pragma once

#include <nlohmann/json.hpp>
#include <pybind11/pytypes.h>

#include <map>
#include <memory>
#include <string>

namespace mrc::pymrc {

#pragma GCC visibility push(default)
/**
 * @brief Immutable container for holding Python values as JSON objects if possible, and as pybind11::object otherwise.
 * The container can be copied and moved, but the underlying JSON object is immutable.
 **/
class PYBIND11_EXPORT JSONValues
{
  public:
    JSONValues() = delete;
    JSONValues(pybind11::object values);

    JSONValues(const JSONValues& other) = default;
    JSONValues(JSONValues&& other)      = default;
    ~JSONValues()                       = default;

    pybind11::object to_python() const;

  private:
    nlohmann::json unserializable_handler(const pybind11::object& obj, const std::string& path);

    nlohmann::json m_serialized_values;
    std::map<std::string, pybind11::object> m_py_objects;
};

#pragma GCC visibility pop
}  // namespace mrc::pymrc