
# This file was generated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_executable(unittester-core-net)
target_compile_options(unittester-core-net PRIVATE
  -Wdeprecated-this-capture
)
target_link_libraries(unittester-core-net PUBLIC
  contrib-libs-linux-headers
  contrib-libs-cxxsupp
  yutil
  library-cpp-cpuid_check
  cpp-testing-gtest
  cpp-testing-gtest_main
  yt-yt-core
  yt-core-test_framework
  cpp-streams-zstd
)
target_link_options(unittester-core-net PRIVATE
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
target_sources(unittester-core-net PRIVATE
  ${CMAKE_SOURCE_DIR}/yt/yt/core/net/unittests/local_address_ut.cpp
  ${CMAKE_SOURCE_DIR}/yt/yt/core/net/unittests/net_ut.cpp
  ${CMAKE_SOURCE_DIR}/yt/yt/core/net/unittests/network_address_ut.cpp
)
add_test(
  NAME
  unittester-core-net
  COMMAND
  unittester-core-net
)
set_property(
  TEST
  unittester-core-net
  PROPERTY
  LABELS
  SMALL
)
set_property(
  TEST
  unittester-core-net
  PROPERTY
  PROCESSORS
  4
)
target_allocator(unittester-core-net
  cpp-ytalloc-impl
)
vcs_info(unittester-core-net)
