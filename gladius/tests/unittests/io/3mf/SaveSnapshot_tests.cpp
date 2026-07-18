#include "EventLogger.h"
#include "io/3mf/Lib3mfLoader.h"
#include "io/3mf/SaveSnapshot.h"
#include "io/3mf/Writer3mf.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace gladius_tests
{

    size_t countResources(Lib3MF::PModel const & model)
    {
        auto resources = model->GetResources();
        size_t count = 0;
        while (resources->MoveNext())
        {
            ++count;
        }
        return count;
    }

    TEST(SaveSnapshotTest, Lib3mfWorkingCopy_IsIndependentFromSource)
    {
        auto wrapper = gladius::io::loadLib3mfScoped();
        auto source = wrapper->CreateModel();
        source->AddImplicitFunction();

        auto sourceWriter = source->QueryWriter("3mf");
        std::vector<Lib3MF_uint8> buffer;
        sourceWriter->WriteToBuffer(buffer);

        auto copy = wrapper->CreateModel();
        auto copyReader = copy->QueryReader("3mf");
        copyReader->ReadFromBuffer(buffer);

        auto const sourceCount = countResources(source);
        copy->AddImplicitFunction();

        EXPECT_EQ(countResources(source), sourceCount);
        EXPECT_GT(countResources(copy), sourceCount);
    }

    TEST(SaveSnapshotTest, Writer_UsesSnapshotAssemblyAndModel)
    {
        auto wrapper = gladius::io::loadLib3mfScoped();
        auto model = wrapper->CreateModel();
        auto assembly = std::make_shared<gladius::nodes::Assembly>();
        assembly->assemblyModel()->createValidVoid();

        gladius::io::SaveSnapshot snapshot;
        snapshot.assembly = assembly;
        snapshot.model = model;

        auto const output = std::filesystem::temp_directory_path() / "gladius_snapshot_test.3mf";
        auto logger = std::make_shared<gladius::events::Logger>(
          gladius::events::OutputMode::Console);
        gladius::io::Writer3mf writer(logger);

        ASSERT_TRUE(writer.save(output, snapshot, false));
        EXPECT_TRUE(std::filesystem::exists(output));
        EXPECT_GT(std::filesystem::file_size(output), 0U);

        std::filesystem::remove(output);
    }

} // namespace gladius_tests
