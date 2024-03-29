cmake_minimum_required(VERSION 3.8)
project(dynarmic C CXX)

# Determine if we're built as a subproject (using add_subdirectory)
# or if this is the master project.
set(MASTER_PROJECT OFF)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(MASTER_PROJECT ON)
endif()

# Add the module directory to the list of paths
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/CMakeModules")

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 12)
    string(APPEND CMAKE_CXX_FLAGS " -fbracket-depth=1024")
endif()

# Arch detection
include(DetectArchitecture)
if (MSVC)
    detect_architecture("_M_AMD64" x86_64)
    detect_architecture("_M_ARM64" Aarch64)
else()
    detect_architecture("__x86_64__" x86_64)
    detect_architecture("__aarch64__" Aarch64)
endif()
if (NOT DEFINED DYNARMIC_ARCHITECTURE)
    message(FATAL_ERROR "Unsupported architecture encountered. Ending CMake generation.")
endif()
message(STATUS "Target architecture: ${DYNARMIC_ARCHITECTURE}")

set(REQUIRES_NO_EXECUTE_SUPPORT OFF)
#Apple Silicon chips require W^X
if(APPLE AND ARCHITECTURE_Aarch64)
    set(REQUIRES_NO_EXECUTE_SUPPORT ON)
endif()

# Dynarmic project options
option(DYNARMIC_ENABLE_CPU_FEATURE_DETECTION "Turning this off causes dynarmic to assume the host CPU doesn't support anything later than SSE3" ON)
option(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT "Enables support for systems that require W^X" ${REQUIRES_NO_EXECUTE_SUPPORT})
option(DYNARMIC_FATAL_ERRORS "Errors are fatal" OFF)
option(DYNARMIC_TESTS "Build tests" ${MASTER_PROJECT})
option(DYNARMIC_TESTS_USE_UNICORN "Enable fuzzing tests against unicorn" OFF)
option(DYNARMIC_USE_LLVM "Support disassembly of jitted x86_64 code using LLVM" OFF)
option(DYNARMIC_WARNINGS_AS_ERRORS "Warnings as errors" ${MASTER_PROJECT})
if (NOT DEFINED DYNARMIC_FRONTENDS)
    set(DYNARMIC_FRONTENDS "A32;A64" CACHE STRING "Selects which frontends to enable")
endif()

# Default to a Release build
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
    message(STATUS "Defaulting to a Release build")
endif()

# Set hard requirements for C++
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Disable in-source builds
set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)
if ("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    message(SEND_ERROR "In-source builds are not allowed.")
endif()

# Compiler flags
if (MSVC)
    set(DYNARMIC_CXX_FLAGS
        /std:c++latest # CMAKE_CXX_STANDARD has no effect on MSVC until CMake 3.10.
        /experimental:external
        /external:W0
        /external:anglebrackets
        /W4
        /w44263 # Non-virtual member function hides base class virtual function
        /w44265 # Class has virtual functions, but destructor is not virtual
        /w44456 # Declaration of 'var' hides previous local declaration
        /w44457 # Declaration of 'var' hides function parameter
        /w44458 # Declaration of 'var' hides class member
        /w44459 # Declaration of 'var' hides global definition
        /w44946 # Reinterpret-cast between related types
        /wd4592 # Symbol will be dynamically initialized (implementation limitation)
        /permissive- # Stricter C++ standards conformance
        /MP
        /Zi
        /Zo
        /EHsc
        /Zc:externConstexpr # Allows external linkage for variables declared "extern constexpr", as the standard permits.
        /Zc:inline          # Omits inline functions from object-file output.
        /Zc:throwingNew     # Assumes new (without std::nothrow) never returns null.
        /volatile:iso       # Use strict standard-abiding volatile semantics
        /bigobj             # Increase number of sections in .obj files
        /DNOMINMAX)

    if (DYNARMIC_WARNINGS_AS_ERRORS)
        list(APPEND DYNARMIC_CXX_FLAGS
             /WX)
    endif()

    if (CMAKE_VS_PLATFORM_TOOLSET MATCHES "LLVM-vs[0-9]+")
        list(APPEND DYNARMIC_CXX_FLAGS
             -Qunused-arguments
             -Wno-missing-braces)
    endif()
else()
    set(DYNARMIC_CXX_FLAGS
        -Wall
        -Wextra
        -Wcast-qual
        -pedantic
        -pedantic-errors
        -Wno-missing-braces)

    if (DYNARMIC_WARNINGS_AS_ERRORS)
        list(APPEND DYNARMIC_CXX_FLAGS
             -Werror)
    endif()

    if (DYNARMIC_FATAL_ERRORS)
        list(APPEND DYNARMIC_CXX_FLAGS
             -Wfatal-errors)
    endif()
endif()

# Include Boost
if (NOT TARGET boost)
    if (NOT Boost_INCLUDE_DIRS)
        find_package(Boost 1.57.0 REQUIRED)
    endif()
    add_library(boost INTERFACE)
    target_include_directories(boost SYSTEM INTERFACE ${Boost_INCLUDE_DIRS})
endif()

# Enable unit-testing.
enable_testing(true)

# Include LLVM
if (DYNARMIC_USE_LLVM)
    find_package(LLVM REQUIRED CONFIG)
    include_directories(${LLVM_INCLUDE_DIRS})
    add_definitions(-DDYNARMIC_USE_LLVM ${LLVM_DEFINITIONS})
    llvm_map_components_to_libnames(llvm_libs armdesc armdisassembler aarch64desc aarch64disassembler x86desc x86disassembler)
endif()

if (DYNARMIC_TESTS_USE_UNICORN)
    find_package(Unicorn REQUIRED)
endif()

# Pull in externals CMakeLists for libs where available
add_subdirectory(externals)

# Dynarmic project files
add_subdirectory(src)
if (DYNARMIC_TESTS)
    add_subdirectory(tests)
endif()
