find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(archive-manager-tests archive_test.cpp)
target_link_libraries(archive-manager-tests PRIVATE archive-core GTest::gtest_main)
target_compile_definitions(archive-manager-tests PRIVATE
    ARCHIVE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
gtest_discover_tests(archive-manager-tests)

add_executable(archive-manager-capture capture.cpp)
target_link_libraries(archive-manager-capture PRIVATE Lingmo::SDK archive-core)
target_compile_definitions(archive-manager-capture PRIVATE
    ARCHIVE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
