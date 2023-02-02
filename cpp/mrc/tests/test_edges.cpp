/**
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

#include "test_mrc.hpp"  // IWYU pragma: associated

#include "mrc/channel/buffered_channel.hpp"  // IWYU pragma: keep
#include "mrc/channel/forward.hpp"
#include "mrc/edge/edge_builder.hpp"
#include "mrc/edge/edge_channel.hpp"
#include "mrc/edge/edge_readable.hpp"
#include "mrc/edge/edge_writable.hpp"
#include "mrc/node/generic_source.hpp"
#include "mrc/node/operators/broadcast.hpp"
#include "mrc/node/operators/combine_latest.hpp"
#include "mrc/node/operators/node_component.hpp"
#include "mrc/node/operators/router.hpp"
#include "mrc/node/sink_channel_owner.hpp"
#include "mrc/node/sink_properties.hpp"
#include "mrc/node/source_channel_owner.hpp"
#include "mrc/node/source_properties.hpp"

#include <glog/logging.h>
#include <gtest/gtest.h>
#include <gtest/internal/gtest-internal.h>

#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

// IWYU pragma: no_forward_declare mrc::channel::Channel

// IWYU thinks we need vector for make_segment
// IWYU pragma: no_include <vector>

using namespace std::chrono_literals;

TEST_CLASS(Edges);

using TestEdgesDeathTest = TestEdges;  // NOLINT(readability-identifier-naming)

namespace mrc::node {

template <typename T>
class EdgeReadableLambda : public edge::IEdgeReadable<T>
{
  public:
    EdgeReadableLambda(std::function<channel::Status(T&)>&& on_await_read,
                       std::function<void()>&& on_complete = nullptr) :
      m_on_await_read(std::move(on_await_read)),
      m_on_complete(std::move(on_complete))
    {}

    ~EdgeReadableLambda() override
    {
        if (m_on_complete)
        {
            m_on_complete();
        }
    }

    channel::Status await_read(T& t) override
    {
        return m_on_await_read(t);
    }

  private:
    std::function<channel::Status(T&)> m_on_await_read;
    std::function<void()> m_on_complete;
};

template <typename T>
class EdgeWritableLambda : public edge::IEdgeWritable<T>
{
  public:
    EdgeWritableLambda(std::function<channel::Status(T&&)>&& on_await_write,
                       std::function<void()>&& on_complete = nullptr) :
      m_on_await_write(std::move(on_await_write)),
      m_on_complete(std::move(on_complete))
    {}

    ~EdgeWritableLambda() override
    {
        if (m_on_complete)
        {
            m_on_complete();
        }
    }

    channel::Status await_write(T&& t) override
    {
        return m_on_await_write(std::move(t));
    }

  private:
    std::function<channel::Status(T&&)> m_on_await_write;
    std::function<void()> m_on_complete;
};

template <typename T>
class TestSource : public WritableAcceptor<T>, public ReadableProvider<T>, public SourceChannelOwner<T>
{
  public:
    TestSource()
    {
        this->set_channel(std::make_unique<mrc::channel::BufferedChannel<T>>());
    }

    void run()
    {
        auto output = this->get_writable_edge();

        for (int i = 0; i < 3; i++)
        {
            if (output->await_write(T(i)) != channel::Status::success)
            {
                break;
            }
        }

        this->release_edge_connection();
    }
};

template <typename T>
class TestNode : public WritableProvider<T>,
                 public ReadableAcceptor<T>,
                 public WritableAcceptor<T>,
                 public ReadableProvider<T>,
                 public SinkChannelOwner<T>,
                 public SourceChannelOwner<T>
{
  public:
    TestNode()
    {
        this->set_channel(std::make_unique<mrc::channel::BufferedChannel<T>>());
    }

    void set_channel(std::unique_ptr<mrc::channel::Channel<T>> channel)
    {
        edge::EdgeChannel<T> edge_channel(std::move(channel));

        SinkChannelOwner<T>::do_set_channel(edge_channel);
        SourceChannelOwner<T>::do_set_channel(edge_channel);
    }

    void run()
    {
        auto input  = this->get_readable_edge();
        auto output = this->get_writable_edge();

        int t;

        while (input->await_read(t) == channel::Status::success)
        {
            VLOG(10) << "Node got value: " << t;

            output->await_write(std::move(t));
        }

        VLOG(10) << "Node exited run";

        SinkChannelOwner<T>::release_edge_connection();
        SourceChannelOwner<T>::release_edge_connection();
    }
};

template <typename T>
class TestSink : public WritableProvider<T>, public ReadableAcceptor<T>, public SinkChannelOwner<T>
{
  public:
    TestSink()
    {
        this->set_channel(std::make_unique<mrc::channel::BufferedChannel<T>>());
    }

    void run()
    {
        auto input = this->get_readable_edge();

        T t;

        while (input->await_read(t) == channel::Status::success)
        {
            VLOG(10) << "Sink got value";
        }

        VLOG(10) << "Sink exited run";

        this->release_edge_connection();
    }
};

template <typename T>
class TestQueue : public WritableProvider<T>, public ReadableProvider<T>
{
  public:
    TestQueue()
    {
        this->set_channel(std::make_unique<mrc::channel::BufferedChannel<T>>());
    }

    void set_channel(std::unique_ptr<mrc::channel::Channel<T>> channel)
    {
        edge::EdgeChannel<T> edge_channel(std::move(channel));

        SinkProperties<T>::init_owned_edge(edge_channel.get_writer());
        SourceProperties<T>::init_owned_edge(edge_channel.get_reader());
    }
};

template <typename T>
class TestSourceComponent : public GenericSourceComponent<T>
{
  public:
    TestSourceComponent() = default;

  protected:
    channel::Status get_data(T& data) override
    {
        data = m_value++;

        VLOG(10) << "TestSourceComponent emmitted value: " << data;

        // Close after 3
        return m_value >= 3 ? channel::Status::closed : channel::Status::success;
    }

    void on_complete() override
    {
        VLOG(10) << "TestSourceComponent completed";
    }

  private:
    T m_value{1};
};

template <typename T>
class TestNodeComponent : public NodeComponent<T, T>
{
  public:
    TestNodeComponent() = default;

    ~TestNodeComponent() override
    {
        // Debug print
        VLOG(10) << "Destroying TestNodeComponent";
    }

    channel::Status on_next(int&& t) override
    {
        VLOG(10) << "TestNodeComponent got value: " << t;

        return this->get_writable_edge()->await_write(t + 1);
    }

    void do_on_complete() override
    {
        VLOG(10) << "TestSinkComponent completed";
    }
};

template <typename T>
class TestSinkComponent : public WritableProvider<T>
{
  public:
    TestSinkComponent()
    {
        this->init_owned_edge(std::make_shared<EdgeWritableLambda<T>>(
            [this](int&& t) {
                // Call this object
                return this->await_write(std::move(t));
            },
            [this]() {
                this->on_complete();
            }));
    }

    channel::Status await_write(int&& t)
    {
        VLOG(10) << "TestSinkComponent got value: " << t;

        return channel::Status::success;
    }

    void on_complete()
    {
        VLOG(10) << "TestSinkComponent completed";
    }
};

template <typename T>
class TestRouter : public Router<std::string, int>
{
  protected:
    std::string determine_key_for_value(const int& t) override
    {
        return t % 2 == 1 ? "odd" : "even";
    }
};

template <typename T>
class TestConditional : public ForwardingWritableProvider<T>, public WritableAcceptor<T>
{
  public:
    TestConditional() = default;

    ~TestConditional() override
    {
        // Debug print
        VLOG(10) << "Destroying TestConditional";
    }

    channel::Status on_next(T&& t) override
    {
        VLOG(10) << "TestConditional got value: " << t;

        // Skip on condition
        if (t % 2 == 0)
        {
            return channel::Status::success;
        }

        return this->get_writable_edge()->await_write(t + 1);
    }

    void on_complete() override
    {
        VLOG(10) << "TestConditional completed";

        WritableAcceptor<T>::release_edge_connection();
    }
};

}  // namespace mrc::node

namespace mrc {

TEST_F(TestEdgesDeathTest, NodeDestroyedBeforeEdge)
{
    // Reset the sink before the source which will cause an exception
    EXPECT_DEATH(
        {
            auto source = std::make_shared<node::TestSource<int>>();
            auto sink   = std::make_shared<node::TestSink<int>>();

            mrc::make_edge(*source, *sink);
            sink.reset();
        },
        "");
}

TEST_F(TestEdges, SourceToSink)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto sink   = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *sink);

    source->run();
    sink->run();
}

TEST_F(TestEdges, SourceToSinkUpcast)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto sink   = std::make_shared<node::TestSink<float>>();

    mrc::make_edge(*source, *sink);

    source->run();
    sink->run();
}

TEST_F(TestEdges, SourceToSinkTypeless)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto sink   = std::make_shared<node::TestSink<int>>();

    mrc::make_edge_typeless(*source, *sink);

    source->run();
    sink->run();
}

TEST_F(TestEdges, SourceToNodeToSink)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto node   = std::make_shared<node::TestNode<int>>();
    auto sink   = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *node);
    mrc::make_edge(*node, *sink);

    source->run();
    node->run();
    sink->run();
}

TEST_F(TestEdges, SourceToNodeToNodeToSink)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto node1  = std::make_shared<node::TestNode<int>>();
    auto node2  = std::make_shared<node::TestNode<int>>();
    auto sink   = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *node1);
    mrc::make_edge(*node1, *node2);
    mrc::make_edge(*node2, *sink);

    source->run();
    node1->run();
    node2->run();
    sink->run();
}

TEST_F(TestEdges, SourceToSinkMultiFail)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto sink1  = std::make_shared<node::TestSink<int>>();
    auto sink2  = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *sink1);
    EXPECT_THROW(mrc::make_edge(*source, *sink2), std::runtime_error);

    source.reset();
    sink1.reset();
    sink2.reset();
}

TEST_F(TestEdges, SourceToSinkComponent)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto sink   = std::make_shared<node::TestSinkComponent<int>>();

    mrc::make_edge(*source, *sink);

    source->run();
}

TEST_F(TestEdges, SourceComponentToSink)
{
    auto source = std::make_shared<node::TestSourceComponent<int>>();
    auto sink   = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *sink);

    sink->run();
}

TEST_F(TestEdges, SourceComponentToNodeToSink)
{
    auto source = std::make_shared<node::TestSourceComponent<int>>();
    auto node   = std::make_shared<node::TestNode<int>>();
    auto sink   = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *node);
    mrc::make_edge(*node, *sink);

    node->run();
    sink->run();
}

TEST_F(TestEdges, SourceToNodeComponentToSink)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto node   = std::make_shared<node::NodeComponent<int>>();
    auto sink   = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *node);
    mrc::make_edge(*node, *sink);

    source->run();
    sink->run();
}

TEST_F(TestEdges, SourceToNodeToSinkComponent)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto node   = std::make_shared<node::TestNode<int>>();
    auto sink   = std::make_shared<node::TestSinkComponent<int>>();

    mrc::make_edge(*source, *node);
    mrc::make_edge(*node, *sink);

    source->run();
    node->run();
}

TEST_F(TestEdges, SourceToNodeComponentToSinkComponent)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto node   = std::make_shared<node::TestNodeComponent<int>>();
    auto sink   = std::make_shared<node::TestSinkComponent<int>>();

    mrc::make_edge(*source, *node);
    mrc::make_edge(*node, *sink);

    source->run();
}

TEST_F(TestEdges, SourceComponentToNodeToSinkComponent)
{
    auto source = std::make_shared<node::TestSourceComponent<int>>();
    auto node   = std::make_shared<node::TestNode<int>>();
    auto sink   = std::make_shared<node::TestSinkComponent<int>>();

    mrc::make_edge(*source, *node);
    mrc::make_edge(*node, *sink);

    node->run();
}

TEST_F(TestEdges, SourceToQueueToSink)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto queue  = std::make_shared<node::TestQueue<int>>();
    auto sink   = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *queue);
    mrc::make_edge(*queue, *sink);

    source->run();
    sink->run();
}

TEST_F(TestEdges, SourceToQueueToNodeToSink)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto queue  = std::make_shared<node::TestQueue<int>>();
    auto node   = std::make_shared<node::TestNode<int>>();
    auto sink   = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *queue);
    mrc::make_edge(*queue, *node);
    mrc::make_edge(*node, *sink);

    source->run();
    node->run();
    sink->run();
}

TEST_F(TestEdges, SourceToQueueToMultiSink)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto queue  = std::make_shared<node::TestQueue<int>>();
    auto sink1  = std::make_shared<node::TestSink<int>>();
    auto sink2  = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *queue);
    mrc::make_edge(*queue, *sink1);
    mrc::make_edge(*queue, *sink2);

    source->run();
    sink1->run();
    sink2->run();
}

TEST_F(TestEdges, SourceToQueueToDifferentSinks)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto queue  = std::make_shared<node::TestQueue<int>>();
    auto sink1  = std::make_shared<node::TestSink<int>>();
    auto node   = std::make_shared<node::TestNode<int>>();
    auto sink2  = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *queue);
    mrc::make_edge(*queue, *sink1);
    mrc::make_edge(*queue, *node);
    mrc::make_edge(*node, *sink2);

    source->run();
    node->run();
    sink1->run();
    sink2->run();
}

TEST_F(TestEdges, SourceToRouterToSinks)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto router = std::make_shared<node::TestRouter<int>>();
    auto sink1  = std::make_shared<node::TestSink<int>>();
    auto sink2  = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *router);
    mrc::make_edge(*router->get_source("odd"), *sink1);
    mrc::make_edge(*router->get_source("even"), *sink2);

    source->run();
    sink1->run();
    sink2->run();
}

TEST_F(TestEdges, SourceToRouterToDifferentSinks)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto router = std::make_shared<node::TestRouter<int>>();
    auto sink1  = std::make_shared<node::TestSink<int>>();
    auto sink2  = std::make_shared<node::TestSinkComponent<int>>();

    mrc::make_edge(*source, *router);
    mrc::make_edge(*router->get_source("odd"), *sink1);
    mrc::make_edge(*router->get_source("even"), *sink2);

    source->run();
    sink1->run();
}

TEST_F(TestEdges, SourceToBroadcastToSink)
{
    auto source    = std::make_shared<node::TestSource<int>>();
    auto broadcast = std::make_shared<node::Broadcast<int>>();
    auto sink      = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *broadcast);
    mrc::make_edge(*broadcast, *sink);

    source->run();
    sink->run();
}

TEST_F(TestEdges, SourceToBroadcastTypelessToSinkSinkFirst)
{
    auto source    = std::make_shared<node::TestSource<int>>();
    auto broadcast = std::make_shared<node::BroadcastTypeless>();
    auto sink      = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*broadcast, *sink);
    mrc::make_edge(*source, *broadcast);

    source->run();
    sink->run();
}

TEST_F(TestEdges, SourceToBroadcastTypelessToSinkSourceFirst)
{
    auto source    = std::make_shared<node::TestSource<int>>();
    auto broadcast = std::make_shared<node::BroadcastTypeless>();
    auto sink      = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *broadcast);
    mrc::make_edge(*broadcast, *sink);

    source->run();
    sink->run();
}

TEST_F(TestEdges, SourceToMultipleBroadcastTypelessToSinkSinkFirst)
{
    auto source     = std::make_shared<node::TestSource<int>>();
    auto broadcast1 = std::make_shared<node::BroadcastTypeless>();
    auto broadcast2 = std::make_shared<node::BroadcastTypeless>();
    auto sink       = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*broadcast2, *sink);
    mrc::make_edge(*broadcast1, *broadcast2);
    mrc::make_edge(*source, *broadcast1);

    source->run();
    sink->run();
}

TEST_F(TestEdges, SourceToMultipleBroadcastTypelessToSinkSourceFirst)
{
    auto source     = std::make_shared<node::TestSource<int>>();
    auto broadcast1 = std::make_shared<node::BroadcastTypeless>();
    auto broadcast2 = std::make_shared<node::BroadcastTypeless>();
    auto sink       = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *broadcast1);
    mrc::make_edge(*broadcast1, *broadcast2);
    mrc::make_edge(*broadcast2, *sink);

    source->run();
    sink->run();
}

TEST_F(TestEdges, MultiSourceToMultipleBroadcastTypelessToMultiSink)
{
    auto source1    = std::make_shared<node::TestSource<int>>();
    auto source2    = std::make_shared<node::TestSource<int>>();
    auto broadcast1 = std::make_shared<node::BroadcastTypeless>();
    auto broadcast2 = std::make_shared<node::BroadcastTypeless>();
    auto sink1      = std::make_shared<node::TestSink<int>>();
    auto sink2      = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source1, *broadcast1);
    mrc::make_edge(*source2, *broadcast1);
    mrc::make_edge(*broadcast1, *broadcast2);
    mrc::make_edge(*broadcast2, *sink1);
    mrc::make_edge(*broadcast2, *sink2);

    source1->run();
    source2->run();
    sink1->run();
    sink2->run();
}

TEST_F(TestEdges, SourceToBroadcastToMultiSink)
{
    auto source    = std::make_shared<node::TestSource<int>>();
    auto broadcast = std::make_shared<node::Broadcast<int>>();
    auto sink1     = std::make_shared<node::TestSink<int>>();
    auto sink2     = std::make_shared<node::TestSink<int>>();

    mrc::make_edge(*source, *broadcast);
    mrc::make_edge(*broadcast, *sink1);
    mrc::make_edge(*broadcast, *sink2);

    source->run();
}

TEST_F(TestEdges, SourceToBroadcastToDifferentSinks)
{
    auto source    = std::make_shared<node::TestSource<int>>();
    auto broadcast = std::make_shared<node::Broadcast<int>>();
    auto sink1     = std::make_shared<node::TestSink<int>>();
    auto sink2     = std::make_shared<node::TestSinkComponent<int>>();

    mrc::make_edge(*source, *broadcast);
    mrc::make_edge(*broadcast, *sink1);
    mrc::make_edge(*broadcast, *sink2);

    source->run();
}

TEST_F(TestEdges, SourceToBroadcastToSinkComponents)
{
    auto source    = std::make_shared<node::TestSource<int>>();
    auto broadcast = std::make_shared<node::Broadcast<int>>();
    auto sink1     = std::make_shared<node::TestSinkComponent<int>>();
    auto sink2     = std::make_shared<node::TestSinkComponent<int>>();

    mrc::make_edge(*source, *broadcast);
    mrc::make_edge(*broadcast, *sink1);
    mrc::make_edge(*broadcast, *sink2);

    source->run();
}

TEST_F(TestEdges, SourceComponentDoubleToSinkFloat)
{
    auto source = std::make_shared<node::TestSourceComponent<double>>();
    auto sink   = std::make_shared<node::TestSink<float>>();

    mrc::make_edge(*source, *sink);

    sink->run();
}

TEST_F(TestEdges, CombineLatest)
{
    auto source1 = std::make_shared<node::TestSource<int>>();
    auto source2 = std::make_shared<node::TestSource<float>>();

    auto combine_latest = std::make_shared<node::CombineLatest<int, float>>();

    auto sink = std::make_shared<node::TestSink<std::tuple<int, float>>>();

    mrc::make_edge(*source1, *combine_latest->get_sink<0>());
    mrc::make_edge(*source2, *combine_latest->get_sink<1>());
    mrc::make_edge(*combine_latest, *sink);

    source1->run();
    source2->run();

    sink->run();
}

TEST_F(TestEdges, SourceToNull)
{
    auto source = std::make_shared<node::TestSource<int>>();

    source->run();
}

TEST_F(TestEdges, SourceToNodeToNull)
{
    auto source = std::make_shared<node::TestSource<int>>();
    auto node   = std::make_shared<node::TestNode<int>>();

    mrc::make_edge(*source, *node);

    source->run();
    node->run();
}

TEST_F(TestEdges, CreateAndDestroy)
{
    {
        auto x = std::make_shared<node::TestSource<int>>();
    }

    {
        auto x = std::make_shared<node::TestNode<int>>();
    }

    {
        auto x = std::make_shared<node::TestSink<int>>();
    }

    {
        auto x = std::make_shared<node::TestSourceComponent<int>>();
    }

    {
        auto x = std::make_shared<node::TestNodeComponent<int>>();
    }

    {
        auto x = std::make_shared<node::TestSinkComponent<int>>();
    }

    {
        auto x = std::make_shared<node::Broadcast<int>>();
    }

    {
        auto x = std::make_shared<node::TestRouter<int>>();
    }

    {
        auto x = std::make_shared<node::TestConditional<int>>();
    }
}

}  // namespace mrc