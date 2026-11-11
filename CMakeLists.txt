cmake_minimum_required(VERSION 3.20)
project(kv-block-manager VERSION 0.1.0
        DESCRIPTION "Paged KV-cache block manager with prefix caching and copy-on-write"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(KVBM_BUILD_TESTS "Build the tests" ON)
option(KVBM_BUILD_BENCH "Build the benchmark and the simulator" ON)
option(KVBM_WERROR "Treat warnings as errors" OFF)
option(KVBM_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(KVBM_COVERAGE "Instrument for coverage" OFF)

if(KVBM_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

if(KVBM_COVERAGE)
  add_compile_options(--coverage -O0)
  add_link_options(--coverage)
endif()

function(kvbm_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast)
  if(KVBM_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

add_library(kvbm src/block_manager.cpp)
add_library(kvbm::kvbm ALIAS kvbm)
target_include_directories(kvbm PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(kvbm PUBLIC cxx_std_20)
kvbm_warnings(kvbm)

install(TARGETS kvbm EXPORT kvbmTargets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)

if(KVBM_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googletest)
  # our warning flags are for our code: treat GoogleTest's headers as system headers
  foreach(gt gtest gtest_main)
    get_target_property(gt_dirs ${gt} INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(${gt} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${gt_dirs}")
  endforeach()

  add_executable(kvbm_tests
    tests/hash_test.cpp
    tests/allocate_test.cpp
    tests/append_test.cpp
    tests/eviction_test.cpp
    tests/collision_test.cpp)
  target_link_libraries(kvbm_tests PRIVATE kvbm GTest::gtest_main)
  kvbm_warnings(kvbm_tests)
  include(GoogleTest)
  gtest_discover_tests(kvbm_tests)
endif()
