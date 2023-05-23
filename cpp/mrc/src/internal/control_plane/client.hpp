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

#pragma once

#include "internal/async_service.hpp"
#include "internal/control_plane/state/root_state.hpp"
#include "internal/grpc/client_streaming.hpp"
#include "internal/grpc/stream_writer.hpp"
#include "internal/resources/iresources_provider.hpp"
#include "internal/resources/partition_resources_base.hpp"
#include "internal/runnable/runnable_resources.hpp"

#include "mrc/core/error.hpp"
#include "mrc/node/forward.hpp"
#include "mrc/node/operators/broadcast.hpp"
#include "mrc/node/operators/conditional.hpp"
#include "mrc/node/writable_entrypoint.hpp"
#include "mrc/protos/architect.grpc.pb.h"
#include "mrc/protos/architect.pb.h"
#include "mrc/runnable/launch_options.hpp"
#include "mrc/types.hpp"
#include "mrc/utils/macros.hpp"

#include <boost/fiber/future/future.hpp>
#include <glog/logging.h>
#include <rxcpp/rx.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// IWYU pragma: no_forward_declare mrc::node::WritableEntrypoint

namespace grpc {
class Channel;
class CompletionQueue;
}  // namespace grpc
namespace mrc::control_plane::client {
class ConnectionsManager;
class SubscriptionService;
}  // namespace mrc::control_plane::client
namespace mrc::network {
class NetworkResources;
}  // namespace mrc::network
namespace mrc::ucx {
class UcxResources;
}  // namespace mrc::ucx
namespace mrc::runnable {
class Runner;
}  // namespace mrc::runnable
namespace mrc::resources {
class IResourcesProvider;
}  // namespace mrc::resources

namespace mrc::control_plane {

template <typename ResponseT>
class AsyncStatus;

/**
 * @brief Primary Control Plane Client
 *
 * A single instance of Client should be instantiated per processes. This class is responsible owning the client side
 * bidirectional async grpc stream, server event handler, and router used to push server side events to partition client
 * event handlers. This class may also create a grpc::CompletionQueue and run a progress engine and progress handler if
 * constructed without an external CQ being passed in. If a CQ is provided, then it is assumed the progress engine and
 * progress handler are also external.
 *
 * The event handler with this class will directly handle ClientErrors, while InstanceErrors will be forward via the
 * event router to the specific instance handler.
 *
 */

// todo: client should be a holder of the stream (private) and the connection manager (public)

class Client final : public AsyncService, public virtual runnable::RunnableResourcesProvider
{
  public:
    enum class State
    {
        Disconnected,
        FailedToConnect,
        Connected,
        RegisteringWorkers,
        Operational,
    };

    using stream_t         = std::shared_ptr<rpc::ClientStream<mrc::protos::Event, mrc::protos::Event>>;
    using writer_t         = std::shared_ptr<rpc::StreamWriter<mrc::protos::Event>>;
    using event_t          = stream_t::element_type::IncomingData;
    using update_channel_t = mrc::node::WritableEntrypoint<protos::StateUpdate>;

    Client(runnable::IRunnableResourcesProvider& resources);

    // Client(resources::PartitionResourceBase& base);

    // if we already have an grpc progress engine running, we don't need run another, just use that cq
    // Client(resources::PartitionResourceBase& base, std::shared_ptr<grpc::CompletionQueue> cq);

    ~Client() final;

    const State& state() const;

    MachineID machine_id() const;
    // const std::vector<InstanceID>& instance_ids() const;

    // InstanceID register_ucx_address(const std::string& worker_address);

    // std::vector<InstanceID> register_ucx_addresses(const std::vector<std::string>& worker_addresses);

    // std::map<InstanceID, std::unique_ptr<client::Instance>> register_ucx_addresses(
    //     std::vector<std::optional<ucx::UcxResources>>& ucx_resources);

    // void register_port_publisher(InstanceID instance_id, const std::string& port_name);
    // void register_port_subscriber(InstanceID instance_id, const std::string& port_name);
    client::SubscriptionService& get_or_create_subscription_service(std::string name, std::set<std::string> roles);

    template <typename ResponseT, typename RequestT>
    Expected<ResponseT> await_unary(const protos::EventType& event_type, RequestT&& request);

    template <typename ResponseT, typename RequestT>
    void async_unary(const protos::EventType& event_type, RequestT&& request, AsyncStatus<ResponseT>& status);

    template <typename MessageT>
    void issue_event(const protos::EventType& event_type, MessageT&& message);

    void issue_event(const protos::EventType& event_type);

    bool has_subscription_service(const std::string& name) const;

    const mrc::runnable::LaunchOptions& launch_options() const;

    // client::ConnectionsManager& connections() const
    // {
    //     CHECK(m_connections_manager);
    //     return *m_connections_manager;
    // }

    // request that the server start an update
    void request_update();

    // edge::IWritableAcceptor<const protos::ControlPlaneState>& state_update_stream() const;

    rxcpp::observable<state::ControlPlaneState> state_update_obs() const;

  private:
    void do_service_start(std::stop_token stop_token) final;
    void do_service_kill() final;

    void route_state_update(std::uint64_t tag, protos::StateUpdate&& update);

    // void do_service_start() final;
    // void do_service_stop() final;
    // void do_service_kill() final;
    // void do_service_await_live() final;
    // void do_service_await_join() final;
    void do_handle_event(event_t& event);

    void forward_state(State state);

    State m_state{State::Disconnected};

    MachineID m_machine_id;
    // std::vector<InstanceID> m_instance_ids;
    // std::map<InstanceID, std::unique_ptr<update_channel_t>> m_update_channels;
    // std::map<InstanceID, std::shared_ptr<client::Instance>> m_instances;

    SharedPromise<void> m_connected_promise;

    std::shared_ptr<grpc::CompletionQueue> m_cq;
    std::shared_ptr<grpc::Channel> m_channel;
    std::shared_ptr<mrc::protos::Architect::Stub> m_stub;

    // if true, then the following runners should not be null
    // if false, then the following runners must be null
    const bool m_owns_progress_engine;
    std::unique_ptr<mrc::runnable::Runner> m_progress_handler;
    std::unique_ptr<AsyncServiceRunnerWrapper> m_progress_handler_wrapper;
    std::unique_ptr<mrc::runnable::Runner> m_progress_engine;
    std::unique_ptr<AsyncServiceRunnerWrapper> m_progress_engine_wrapper;

    std::unique_ptr<node::Conditional<bool, event_t>> m_response_conditional;
    std::unique_ptr<mrc::runnable::Runner> m_response_handler;
    std::unique_ptr<mrc::runnable::Runner> m_event_handler;

    std::unique_ptr<AsyncServiceRunnerWrapper> m_event_handler_wrapper;

    // std::map<std::string, std::unique_ptr<node::SourceChannelWriteable<protos::StateUpdate>>> m_update_channels;
    // std::unique_ptr<client::ConnectionsManager> m_connections_manager;
    // std::map<std::string, std::unique_ptr<client::SubscriptionService>> m_subscription_services;

    // connection manager - connected to the update channel
    // std::unique_ptr<client::ConnectionsManager> m_connections_manager;

    // update channel
    size_t m_state_update_count{0};
    rxcpp::subjects::behavior<state::ControlPlaneState> m_state_update_sub{
        state::ControlPlaneState{std::make_unique<protos::ControlPlaneState>()}};
    // rxcpp::subjects::replay<state::ControlPlaneState, rxcpp::identity_one_worker> m_state_update_sub{
    //     1,
    //     rxcpp::identity_current_thread()};
    // std::unique_ptr<mrc::node::WritableEntrypoint<const protos::StateUpdate>> m_connections_update_channel;
    // std::unique_ptr<mrc::node::WritableEntrypoint<const protos::ControlPlaneState>> m_state_update_entrypoint;
    // std::unique_ptr<mrc::node::Broadcast<const protos::ControlPlaneState>> m_state_update_stream;
    // std::map<InstanceID, mrc::node::WritableEntrypoint<const protos::StateUpdate>> m_instance_update_channels;

    // Stream Context
    stream_t m_stream;

    // StreamWriter acquired from m_stream->await_init()
    // The customer destruction of this object will cause a gRPC WritesDone to be issued to the server.
    writer_t m_writer;

    mrc::runnable::LaunchOptions m_launch_options;

    std::mutex m_mutex;

    friend network::NetworkResources;
};

// todo: create this object from the client which will own the stop_source
// create this object with a stop_token associated with the client's stop_source

template <typename ResponseT>
class AsyncStatus
{
  public:
    AsyncStatus() = default;

    DELETE_COPYABILITY(AsyncStatus);
    DELETE_MOVEABILITY(AsyncStatus);

    Expected<ResponseT> await_response()
    {
        // todo(ryan): expand this into a wait_until with a deadline and a stop token
        auto event = m_promise.get_future().get();

        if (event.has_error())
        {
            return Error::create(event.error().message());
        }

        ResponseT response;
        if (!event.message().UnpackTo(&response))
        {
            throw Error::create("fatal error: unable to unpack message; server sent the wrong message type");
        }

        return response;
    }

  private:
    Promise<protos::Event> m_promise;
    friend Client;
};

template <typename ResponseT, typename RequestT>
Expected<ResponseT> Client::await_unary(const protos::EventType& event_type, RequestT&& request)
{
    AsyncStatus<ResponseT> status;
    async_unary(event_type, std::move(request), status);
    return status.await_response();
}

template <typename ResponseT, typename RequestT>
void Client::async_unary(const protos::EventType& event_type, RequestT&& request, AsyncStatus<ResponseT>& status)
{
    protos::Event event;
    event.set_event(event_type);
    event.set_tag(reinterpret_cast<std::uint64_t>(&status.m_promise));
    CHECK(event.mutable_message()->PackFrom(request));
    m_writer->await_write(std::move(event));
}

template <typename MessageT>
void Client::issue_event(const protos::EventType& event_type, MessageT&& message)
{
    protos::Event event;
    event.set_event(event_type);
    CHECK(event.mutable_message()->PackFrom(message));
    m_writer->await_write(std::move(event));
}

}  // namespace mrc::control_plane
