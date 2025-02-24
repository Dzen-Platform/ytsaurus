
# This file was generated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(helpers)

add_executable(unittester-client-table-client)
target_compile_options(unittester-client-table-client PRIVATE
  -Wdeprecated-this-capture
)
target_link_libraries(unittester-client-table-client PUBLIC
  contrib-libs-linux-headers
  contrib-libs-cxxsupp
  yutil
  library-cpp-cpuid_check
  cpp-testing-gtest
  cpp-testing-gtest_main
  yt-yt-client
  yt-library-formats
  table_client-unittests-helpers
  client-unittests-mock
  yt-core-test_framework
)
target_link_options(unittester-client-table-client PRIVATE
  -ldl
  -lrt
  -Wl,--no-as-needed
  -fPIC
  -fPIC
  -lpthread
  -lrt
  -ldl
  -lutil
)
target_sources(unittester-client-table-client PRIVATE
  ${CMAKE_SOURCE_DIR}/yt/yt/client/table_client/unittests/columnar_statistics_ut.cpp
  ${CMAKE_SOURCE_DIR}/yt/yt/client/table_client/unittests/columnar_ut.cpp
  ${CMAKE_SOURCE_DIR}/yt/yt/client/table_client/unittests/serialization_ut.cpp
)
add_test(
  NAME
  unittester-client-table-client
  COMMAND
  unittester-client-table-client
)
set_property(
  TEST
  unittester-client-table-client
  PROPERTY
  LABELS
  MEDIUM
)
set_property(
  TEST
  unittester-client-table-client
  PROPERTY
  PROCESSORS
  1
)
target_allocator(unittester-client-table-client
  cpp-ytalloc-impl
)
vcs_info(unittester-client-table-client)
