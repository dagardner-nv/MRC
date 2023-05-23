/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "internal/executor/executor_definition.hpp"

#include "internal/pipeline/manager.hpp"
#include "internal/pipeline/pipeline_definition.hpp"
#include "internal/pipeline/port_graph.hpp"
#include "internal/pipeline/types.hpp"
#include "internal/runtime/runtime.hpp"
#include "internal/system/system.hpp"

#include "mrc/core/addresses.hpp"
#include "mrc/exceptions/runtime_error.hpp"

#include <glog/logging.h>

#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <utility>

namespace mrc::executor {

static bool valid_pipeline(const pipeline::PipelineDefinition& pipeline)
{
    bool valid = true;
    pipeline::PortGraph pg(pipeline);

    for (const auto& [name, connections] : pg.port_map())
    {
        // first validate all port names have at least one:
        // - segment using that port as an ingress, and
        // - segment using that port as an egress
        if (connections.egress_segments.empty() or connections.ingress_segments.empty())
        {
            valid = false;
            // todo - print list of segments names for ingress/egres connections to this port
            LOG(WARNING) << "port: " << name << " has incomplete connections - used as ingress on "
                         << connections.ingress_segments.size() << " segments; used as egress on "
                         << connections.egress_segments.size() << " segments";
        }

        // we currently only have an load-balancer manifold
        // it doesn't make sense to connect segments of different types to a load-balancer, they should probably be
        // broadcast
        // in general, if there are more than one type of segments writing to or reading from a manifold, then that port
        // should have an explicit manifold type specified
        if (connections.egress_segments.size() > 1 or connections.ingress_segments.size() > 1)
        {
            valid = false;
            LOG(WARNING) << "port: " << name
                         << " has more than 1 segment type connected to an ingress or egress port; this is currently "
                            "an invalid configuration as there are no manifold available to handle this condition";
        }
    }

    return valid;
}

ExecutorDefinition::ExecutorDefinition(std::unique_ptr<system::SystemDefinition> system) :
  SystemProvider(std::move(system))
{}

// ExecutorDefinition::ExecutorDefinition(std::unique_ptr<system::ThreadingResources> resources) :
//   SystemProvider(*resources),
//   m_resources_manager(std::make_unique<resources::Manager>(std::move(resources)))
// {}

ExecutorDefinition::~ExecutorDefinition()
{
    Service::call_in_destructor();
}

std::shared_ptr<ExecutorDefinition> ExecutorDefinition::unwrap(std::shared_ptr<pipeline::IExecutor> object)
{
    // Convert to the full implementation
    auto full_object = std::dynamic_pointer_cast<ExecutorDefinition>(object);

    CHECK(full_object) << "Invalid cast for ExecutorDefinition. Please report to the developers";

    return full_object;
}

void ExecutorDefinition::register_pipeline(std::shared_ptr<pipeline::IPipeline> pipeline)
{
    CHECK(pipeline);

    auto full_pipeline = pipeline::PipelineDefinition::unwrap(std::move(pipeline));

    if (!valid_pipeline(*full_pipeline))
    {
        throw exceptions::MrcRuntimeError("pipeline validation failed");
    }

    // m_pipeline_manager = std::make_unique<pipeline::Manager>(pipeline, *m_resources_manager);

    m_registered_pipeline_defs.emplace_back(std::move(full_pipeline));
}

void ExecutorDefinition::start()
{
    this->service_start();
}

void ExecutorDefinition::stop()
{
    this->service_stop();
}

void ExecutorDefinition::join()
{
    this->service_await_join();
}

void ExecutorDefinition::do_service_start()
{
    // Get a lock on the pipelines
    std::unique_lock<typeof(m_pipelines_mutex)> lock(m_pipelines_mutex);

    m_runtime = std::make_unique<runtime::Runtime>(*this);

    m_runtime->service_start();
    m_runtime->service_await_live();

    // Now move the registered pipelines into the pipelines manager
    m_runtime->pipelines_manager().register_defs(m_registered_pipeline_defs);

    // // Now make the request for which segments to run
    // for (const auto& [pipeline_id, pipeline] : m_registered_pipeline_defs)
    // {
    //     auto request = protos::PipelineRequestAssignmentRequest();
    //     request.set_machine_id(0);
    //     request.set_pipeline_id(0);

    //     for (const auto& [segment_id, segment] : pipeline->segments())
    //     {
    //         auto address = segment_address_encode(segment_id, 0);  // rank 0

    //         (*request.mutable_segment_assignments())[segment_id] = 0;
    //     }

    //     auto response = m_runtime->control_plane().await_unary<protos::PipelineRequestAssignmentResponse>(
    //         protos::EventType::ClientUnaryRequestPipelineAssignment,
    //         request);

    //     // Create a manager for the pipeline in the response
    //     auto pipeline_manager = std::make_shared<pipeline::PipelineManager>(pipeline,
    //                                                                         m_runtime->resources(),
    //                                                                         response->pipeline_id());

    //     // Save to the managers before starting
    //     m_pipeline_managers.push_back(pipeline_manager);

    //     // Create a fiber to join on the pipeline manager
    //     m_runtime->partition(0).resources().runnable().main().enqueue([this, pipeline_manager]() {
    //         // Start the manager
    //         pipeline_manager->service_start();

    //         // Await on joining
    //         pipeline_manager->service_await_join();

    //         // Get a lock on the pipelines
    //         std::unique_lock<typeof(m_pipelines_mutex)> lock(m_pipelines_mutex);

    //         // Remove this from the list of pipelines
    //         m_pipeline_managers.erase(
    //             std::find(m_pipeline_managers.begin(), m_pipeline_managers.end(), pipeline_manager));

    //         // Signal the CV to check if we are done
    //         m_pipelines_cv.notify_all();
    //     });

    //     LOG(INFO) << "Registered pipeline";
    // }

    // // Start by making an edge to the control plane udpates
    // m_update_sink = std::make_unique<node::LambdaSinkComponent<const protos::ControlPlaneState>>(
    //     [this](const protos::ControlPlaneState&& state) -> channel::Status {
    //         // On update, check for pipelines that are different than the current set of pipelines
    //         auto instances = std::vector<protos::PipelineInstance>(state.pipeline_instances().entities().begin(),
    //                                                                state.pipeline_instances().entities().end());

    //         for (const auto& ins : instances)
    //         {
    //             // TEMP: Should check against running instances
    //             if (!m_pipeline_manager)
    //             {
    //                 m_pipeline_manager = std::make_unique<pipeline::Manager>(m_registered_pipeline_defs[0],
    //                                                                          *m_resources_manager);

    //                 m_pipeline_manager->service_start();
    //             }
    //         }

    //         return channel::Status::success;
    //     });

    // // Make an edge between the update and the sink
    // mrc::make_edge(m_resources_manager->control_plane().client().state_update_stream(), *m_update_sink);

    // CHECK(m_pipeline_manager);
    // m_pipeline_manager->service_start();

    // pipeline::SegmentAddresses initial_segments;
    // for (const auto& [id, segment] : m_pipeline_manager->pipeline().segments())
    // {
    //     auto address              = segment_address_encode(id, 0);  // rank 0
    //     initial_segments[address] = 0;                              // partition 0;
    // }
    // m_pipeline_manager->push_updates(std::move(initial_segments));
}

void ExecutorDefinition::do_service_stop()
{
    m_runtime->service_stop();

    // Get a lock on the pipelines
    std::unique_lock<typeof(m_pipelines_mutex)> lock(m_pipelines_mutex);

    for (const auto& pipeline : m_pipeline_managers)
    {
        pipeline->service_stop();
    }
}
void ExecutorDefinition::do_service_kill()
{
    m_runtime->service_kill();

    // // Get a lock on the pipelines
    // std::unique_lock<typeof(m_pipelines_mutex)> lock(m_pipelines_mutex);

    // for (const auto& pipeline : m_pipeline_managers)
    // {
    //     pipeline->service_kill();
    // }
}
void ExecutorDefinition::do_service_await_live()
{
    m_runtime->service_await_live();
    // CHECK(m_pipeline_manager);
    // m_pipeline_manager->service_await_live();
}
void ExecutorDefinition::do_service_await_join()
{
    m_runtime->service_await_join();

    // // CHECK(m_pipeline_manager);

    // // Get a lock on the pipelines
    // std::unique_lock<typeof(m_pipelines_mutex)> lock(m_pipelines_mutex);

    // m_pipelines_cv.wait(lock, [this]() {
    //     return m_pipeline_managers.empty();
    // });

    // DVLOG(10) << "Exiting Executor::do_service_await_join()";
}

}  // namespace mrc::executor
