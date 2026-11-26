/**
 * @file AssemblyTraversalSynchronization_tests.cpp
 * @brief Regression tests for the assembly-mutex synchronization layer that protects node/port
 *        graph traversal (UI thread) from concurrent structural mutation (background/MCP).
 * @details The original crash was a SIGSEGV inside std::type_index::operator== reached from
 *          NodeView::typeToColor while Model::visitNodes iterated NodeBase::m_outputs, a
 *          std::map<PortName, Port> that a background writer rebalanced at the same time.
 *          These tests pin the token contract: traversal and mutation only ever run while an
 *          AssemblyToken is held, requestAssemblyToken() never returns a second owner, and
 *          self-locking Document entry points stay callable right after a token scope ends.
 */

#include "Document.h"
#include "EventLogger.h"
#include "nodes/Assembly.h"
#include "nodes/Model.h"
#include "nodes/NodeBase.h"
#include "nodes/Port.h"
#include "nodes/Visitor.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <typeindex>

namespace gladius::tests
{
    namespace
    {
        /// @brief Coreless document (no ComputeCore); Document is non-movable, so callers seed
        /// the default valid-void graph themselves via newModel().
        [[nodiscard]] Document makeDocument()
        {
            return Document{std::make_shared<events::Logger>(events::OutputMode::Silent)};
        }

        /**
         * @brief Visitor that mirrors NodeView::typeToColor: iterates each node's output port map
         *        and compares the stored std::type_index values.
         */
        class OutputTypeScanner : public nodes::Visitor
        {
          public:
            void visit(nodes::NodeBase & node) override
            {
                for (auto const & [portName, port] : node.getOutputs())
                {
                    auto const typeIndex = port.getTypeIndex();
                    if (typeIndex == std::type_index(typeid(float)))
                    {
                        ++m_scalarOutputs;
                    }
                    else
                    {
                        ++m_otherOutputs;
                    }
                }
            }

            [[nodiscard]] uint64_t totalOutputs() const
            {
                return m_scalarOutputs + m_otherOutputs;
            }

          private:
            uint64_t m_scalarOutputs = 0;
            uint64_t m_otherOutputs = 0;
        };

        /// @brief Counts sections that were entered while another one was still active.
        class OverlapGuard
        {
          public:
            void enter()
            {
                if (m_inFlight.fetch_add(1, std::memory_order_acq_rel) != 0)
                {
                    m_overlapped.store(true, std::memory_order_release);
                }
            }

            void leave()
            {
                m_inFlight.fetch_sub(1, std::memory_order_acq_rel);
            }

            [[nodiscard]] bool overlapped() const
            {
                return m_overlapped.load(std::memory_order_acquire);
            }

          private:
            std::atomic<int> m_inFlight{0};
            std::atomic<bool> m_overlapped{false};
        };

        /// @brief RAII entry/exit pairing for OverlapGuard around a token-held section.
        class ScopedEntry
        {
          public:
            explicit ScopedEntry(OverlapGuard & guard)
                : m_guard(guard)
            {
                m_guard.enter();
            }

            ~ScopedEntry()
            {
                m_guard.leave();
            }

          private:
            OverlapGuard & m_guard;
        };
    } // namespace

    // Graph traversal and structural mutation must never interleave; both acquire the assembly
    // token before touching NodeBase::m_outputs.
    TEST(AssemblyTraversalSynchronization,
         WaitForAssemblyToken_ConcurrentTraversalAndMutation_SerializedWithoutCrash)
    {
        constexpr int ITERATIONS = 200;

        auto document = makeDocument();
        document.newModel();
        auto assembly = document.getAssembly();
        ASSERT_NE(assembly, nullptr);

        OverlapGuard guard;

        // Reader: traverses the whole graph under a token, comparing output port type indices
        // exactly like NodeView::typeToColor does.
        auto reader = std::async(std::launch::async,
                                 [&]
                                 {
                                     OutputTypeScanner scanner;
                                     for (int i = 0; i < ITERATIONS; ++i)
                                     {
                                         auto const token = document.waitForAssemblyToken();
                                         ScopedEntry const entry{guard};
                                         assembly->visitNodes(scanner);
                                     }
                                     return scanner.totalOutputs();
                                 });

        // Writer: re-resolves node input/output types under its own token.
        auto writer = std::async(std::launch::async,
                                 [&]
                                 {
                                     for (int i = 0; i < ITERATIONS; ++i)
                                     {
                                         auto const token = document.waitForAssemblyToken();
                                         ScopedEntry const entry{guard};
                                         assembly->updateInputsAndOutputs();
                                     }
                                 });

        auto const totalOutputs = reader.get();
        writer.get();

        EXPECT_FALSE(guard.overlapped());
        EXPECT_GT(totalOutputs, 0u);
    }

    // The non-blocking request must report contention instead of handing out a second owner.
    TEST(AssemblyTraversalSynchronization,
         RequestAssemblyToken_MutexHeldByOtherThread_ReturnsEmptyToken)
    {
        auto document = makeDocument();
        document.newModel();

        {
            auto const heldToken = document.waitForAssemblyToken();
            auto probe =
              std::async(std::launch::async,
                         [&document] { return document.requestAssemblyToken().has_value(); });
            EXPECT_FALSE(probe.get());
        }

        EXPECT_TRUE(document.requestAssemblyToken().has_value());
    }

    // update3mfModel() locks the assembly mutex itself, so it must stay callable right after a
    // caller's token scope exits (non-recursive mutex: nesting would self-deadlock).
    TEST(AssemblyTraversalSynchronization,
         Update3mfModel_ImmediatelyAfterTokenRelease_DoesNotDeadlock)
    {
        auto document = makeDocument();
        document.newModel();

        {
            auto const token = document.waitForAssemblyToken();
            EXPECT_NE(document.getAssembly(), nullptr);
        }

        auto sync = std::async(std::launch::async, [&document] { document.update3mfModel(); });
        ASSERT_EQ(sync.wait_for(std::chrono::seconds(10)), std::future_status::ready);
        sync.get();
    }
} // namespace gladius::tests
