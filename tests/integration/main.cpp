#include "docker_fixture.h"
#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new pgpp_test::DockerPostgresEnvironment());
    return RUN_ALL_TESTS();
}
