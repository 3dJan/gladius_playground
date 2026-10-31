#include "compute/RenderBackendSession.h"
#include "compute/ParameterLayout.h"
#include "nodes/Assembly.h"

#include <gtest/gtest.h>

#include <utility>

namespace gladius::compute::tests
{
    namespace
    {
        class TestScene final : public IRenderScene
        {
          public:
            explicit TestScene(std::uint64_t sceneGeneration)
                : m_sceneGeneration{sceneGeneration}
            {
            }

            [[nodiscard]] ComputeBackendKind getBackendKind() const noexcept override
            {
                return ComputeBackendKind::WebGPU;
            }

            [[nodiscard]] std::uint64_t getSceneGeneration() const noexcept override
            {
                return m_sceneGeneration;
            }

            [[nodiscard]] RendererCapability getCapabilities() const noexcept override
            {
                return RendererCapability::AnalyticRendering;
            }

            [[nodiscard]] bool updateParameterSnapshot(
              std::shared_ptr<const RenderParameterSnapshot> snapshot) noexcept override
            {
                if (!snapshot || snapshot->sceneGeneration != m_sceneGeneration)
                {
                    return false;
                }
                m_parameterSnapshot = std::move(snapshot);
                return true;
            }

            std::shared_ptr<const RenderParameterSnapshot> m_parameterSnapshot;

          private:
            std::uint64_t m_sceneGeneration;
        };

        class TestSubmission final : public IRenderSubmission
        {
          public:
            [[nodiscard]] RenderSubmissionStatus getStatus() const noexcept override
            {
                return RenderSubmissionStatus::Cancelled;
            }

            void requestCancellation() noexcept override
            {
            }

            void wait() override
            {
            }

            [[nodiscard]] std::optional<RenderFrame> takeFrame() override
            {
                return std::nullopt;
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                return {};
            }
        };

        class TestBoundsService final : public IBoundsService
        {
          public:
            [[nodiscard]] RendererCapability getCapabilities() const noexcept override
            {
                return RendererCapability::BoundingBoxDetermination;
            }

            [[nodiscard]] bool isAvailable() const noexcept override
            {
                return true;
            }

            void setSceneSnapshot(std::shared_ptr<const RenderSceneSnapshot> snapshot) noexcept override
            {
                m_snapshot = std::move(snapshot);
            }

                        void setParameterSnapshot(
                            std::shared_ptr<const RenderParameterSnapshot> snapshot) noexcept override
                        {
                                m_parameterSnapshot = std::move(snapshot);
                        }

            [[nodiscard]] std::optional<BoundsResult>
            getCachedResult(RenderFreshnessStamp const &) const noexcept override
            {
                return std::nullopt;
            }

            [[nodiscard]] std::unique_ptr<IBoundsSubmission> submit(BoundsRequest) override
            {
                throw std::runtime_error("Test bounds service does not submit work");
            }

            std::shared_ptr<const RenderSceneSnapshot> m_snapshot;
            std::shared_ptr<const RenderParameterSnapshot> m_parameterSnapshot;
        };

        class TestRenderer final : public IComputeRenderer
        {
          public:
            [[nodiscard]] ComputeBackendKind getBackendKind() const noexcept override
            {
                return ComputeBackendKind::WebGPU;
            }

            [[nodiscard]] RendererCapability getCapabilities() const noexcept override
            {
                return RendererCapability::AnalyticRendering;
            }

            [[nodiscard]] bool isAvailable() const noexcept override
            {
                return m_available;
            }

            [[nodiscard]] std::unique_ptr<IRenderScene> materializeScene(RenderSceneSnapshot snapshot) override
            {
                ++m_materializationCount;
                if (snapshot.sceneGeneration == m_failingGeneration)
                {
                    throw std::runtime_error("Materialization failed");
                }
                auto scene = std::make_unique<TestScene>(snapshot.sceneGeneration);
                m_scene = scene.get();
                return scene;
            }

            [[nodiscard]] std::unique_ptr<IRenderSubmission>
            submitFrame(IRenderScene const & scene, RenderRequest) override
            {
                m_submittedSceneGeneration = scene.getSceneGeneration();
                return std::make_unique<TestSubmission>();
            }

            [[nodiscard]] std::unique_ptr<IRenderSubmission> submitFrame(RenderRequest) override
            {
                throw std::runtime_error("Scene-aware submission required");
            }

            bool m_available{true};
            std::uint64_t m_failingGeneration{};
            std::uint64_t m_submittedSceneGeneration{};
            std::size_t m_materializationCount{};
            TestScene * m_scene{};
        };

        [[nodiscard]] RenderSceneSnapshot makeSnapshot(std::uint64_t sceneGeneration)
        {
            return {.sceneGeneration = sceneGeneration,
                    .analyticEvaluatorWgsl = "fn evaluateModel(position: vec3<f32>) -> vec4<f32> { return vec4<f32>(position, 0.0); }"};
        }

                [[nodiscard]] nodes::Assembly makeAssemblyWithScalarParameter(
                    nodes::ParameterOrigin & origin)
                {
                        nodes::Assembly assembly;
                        auto model = assembly.assemblyModel();
                        auto * node = model->create<nodes::ConstantScalar>();
                        auto & parameter = node->parameter().at(nodes::FieldNames::Value);
                        parameter.setValue(1.0f);
                        origin = nodes::ParameterOrigin{assembly.getAssemblyModelId(), parameter.getId()};
                        return assembly;
                }

                [[nodiscard]] RenderSceneSnapshot makeRuntimeSnapshot(
                    std::uint64_t const sceneGeneration,
                    nodes::ParameterOrigin const origin)
                {
                        auto snapshot = makeSnapshot(sceneGeneration);
                        snapshot.parameterValues = {1.0f};
                        auto layout = std::make_shared<ParameterLayout>(sceneGeneration);
                        layout->add(origin, 0u, nodes::ParameterTypeIndex::Float);
                        snapshot.parameterLayout = std::move(layout);
                        return snapshot;
                }
    }

    TEST(RenderBackendSession, ReplaceScene_WithValidSnapshot_StoresMaterializedScene)
    {
        auto renderer = std::make_unique<TestRenderer>();
        RenderBackendSession session{std::move(renderer)};

        ASSERT_TRUE(session.replaceScene(makeSnapshot(7u)));

        EXPECT_TRUE(session.hasMaterializedScene());
        EXPECT_EQ(session.getSceneGeneration(), 7u);
        EXPECT_TRUE(session.getErrorMessage().empty());
    }

    TEST(RenderBackendSession, ReplaceScene_WithBoundsService_PublishesSameSnapshot)
    {
        auto renderer = std::make_unique<TestRenderer>();
        auto boundsService = std::make_shared<TestBoundsService>();
        auto const * boundsServicePointer = boundsService.get();
        RenderBackendSession session{std::move(renderer), boundsService};

        ASSERT_TRUE(session.replaceScene(makeSnapshot(7u)));

        ASSERT_NE(session.getSceneSnapshot(), nullptr);
        ASSERT_NE(boundsServicePointer->m_snapshot, nullptr);
        EXPECT_EQ(session.getSceneSnapshot(), boundsServicePointer->m_snapshot);
        EXPECT_EQ(session.getSceneSnapshot()->sceneGeneration, 7u);
        EXPECT_TRUE(hasCapability(session.getCapabilities(), RendererCapability::BoundingBoxDetermination));
    }

    TEST(RenderBackendSession, ReplaceScene_WhenMaterializationFails_RetainsPreviousScene)
    {
        auto renderer = std::make_unique<TestRenderer>();
        auto * rendererPointer = renderer.get();
        RenderBackendSession session{std::move(renderer)};
        ASSERT_TRUE(session.replaceScene(makeSnapshot(7u)));
        rendererPointer->m_failingGeneration = 8u;

        EXPECT_FALSE(session.replaceScene(makeSnapshot(8u)));
        EXPECT_TRUE(session.hasMaterializedScene());
        EXPECT_EQ(session.getSceneGeneration(), 7u);
        EXPECT_EQ(session.getErrorMessage(), "Materialization failed");
    }

    TEST(RenderBackendSession, SubmitFrame_WithMaterializedScene_ForwardsSceneToRenderer)
    {
        auto renderer = std::make_unique<TestRenderer>();
        auto * rendererPointer = renderer.get();
        RenderBackendSession session{std::move(renderer)};
        ASSERT_TRUE(session.replaceScene(makeSnapshot(7u)));

        auto submission = session.submitFrame(
          RenderRequest{.viewport = {.width = 1u, .height = 1u, .firstRow = 0u, .endRow = 1u}});

        ASSERT_NE(submission, nullptr);
        EXPECT_EQ(rendererPointer->m_submittedSceneGeneration, 7u);
    }

    TEST(RenderBackendSession, UpdateParameterValues_PreservesMaterializedScene)
    {
        nodes::ParameterOrigin origin;
        auto assembly = makeAssemblyWithScalarParameter(origin);
        auto renderer = std::make_unique<TestRenderer>();
        auto * rendererPointer = renderer.get();
        auto boundsService = std::make_shared<TestBoundsService>();
        auto const * boundsServicePointer = boundsService.get();
        RenderBackendSession session{std::move(renderer), boundsService};
        ASSERT_TRUE(session.replaceScene(makeRuntimeSnapshot(7u, origin), 3u));
        auto const structuralSnapshot = session.getSceneSnapshot();

        auto model = assembly.assemblyModel();
        auto * parameter = model->getParameter(origin.parameterId);
        ASSERT_NE(parameter, nullptr);
        parameter->setValue(2.5f);

        ASSERT_TRUE(session.updateParameterValues(assembly, 4u)) << session.getErrorMessage();

        EXPECT_EQ(rendererPointer->m_materializationCount, 1u);
        EXPECT_EQ(session.getSceneSnapshot(), structuralSnapshot);
        ASSERT_NE(session.getParameterSnapshot(), nullptr);
        EXPECT_EQ(session.getParameterSnapshot()->sceneGeneration, 7u);
        EXPECT_EQ(session.getParameterSnapshot()->parameterGeneration, 4u);
        ASSERT_EQ(session.getParameterSnapshot()->values.size(), 1u);
        EXPECT_FLOAT_EQ(session.getParameterSnapshot()->values[0], 2.5f);
        ASSERT_NE(rendererPointer->m_scene, nullptr);
        EXPECT_EQ(rendererPointer->m_scene->m_parameterSnapshot,
                  session.getParameterSnapshot());
        EXPECT_EQ(boundsServicePointer->m_snapshot, structuralSnapshot);
        EXPECT_EQ(boundsServicePointer->m_parameterSnapshot, session.getParameterSnapshot());
    }
}
