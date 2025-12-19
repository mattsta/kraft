set(DK_VERSION 0.3.0)
set(DK_ABI_UPDATE 0.3.0)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

#add_subdirectory("deps/kraft")

if(APPLE)
    set(CMAKE_SHARED_MODULE_CREATE_C_FLAGS
        "${CMAKE_SHARED_MODULE_CREATE_C_FLAGS} -undefined dynamic_lookup")
    cmake_policy(SET CMP0042 NEW)
endif()

option(Build32Bit "Build 32-bit Library" OFF)

# Check for libuv - kraft demo requires it
find_package(PkgConfig)
if (PKG_CONFIG_FOUND)
    pkg_check_modules(LIBUV QUIET libuv)
endif()

# Also check common locations for libuv header
if(NOT LIBUV_FOUND)
    find_path(LIBUV_INCLUDE_DIR NAMES uv.h PATHS /usr/local/include /opt/homebrew/include)
    if(LIBUV_INCLUDE_DIR)
        set(LIBUV_FOUND TRUE)
    endif()
endif()

if(NOT LIBUV_FOUND)
    message(STATUS "libuv not found - skipping kraft-demo build")
    return()
endif()

message(STATUS "libuv found - building kraft-demo")

# If you need debug, build with:
# cmake -DCMAKE_BUILD_TYPE=Debug ..
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic -std=gnu17 -Wno-missing-field-initializers")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstrict-aliasing -Wstrict-overflow -Wno-format-pedantic")

if(CMAKE_BUILD_TYPE MATCHES Debug)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -DKRAFT_TEST_VERBOSE")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2")
endif()

add_library(kraft-demo-impl OBJECT
    # state machine updater for testing purposes.
    kraftStateMachine.c

    # Kraft RPC Entry Coder
    entryCoder.c

    # Kraft Network Information Coder
    #    netpack.c

    # Kraft Server
    serverAdapterLibuv.c
    protocolBinary.c
    
    # Run it all
    demo.c)

add_library(kraft-demo-impl-static STATIC $<TARGET_OBJECTS:kraft-demo-impl>)

set_target_properties(kraft-demo-impl-static PROPERTIES PREFIX "") # don't prefix "lib"

target_link_libraries(kraft-demo-impl-static kraft-static)
#target_link_libraries(kraft-demo-impl-static kvidxkit-static)

set(KRAFT_EXECUTABLE kraft-demo)
add_executable(${KRAFT_EXECUTABLE} demo.c)
target_link_libraries(${KRAFT_EXECUTABLE} kraft-static kraft-demo-impl-static xxhash-static)

if (APPLE)
    add_custom_command(TARGET ${KRAFT_EXECUTABLE} POST_BUILD COMMAND dsymutil ${KRAFT_EXECUTABLE} COMMENT "Generating OS X Debug Info")
endif()

# Include libuv directories
if(LIBUV_INCLUDE_DIRS)
    include_directories(SYSTEM ${LIBUV_INCLUDE_DIRS})
elseif(LIBUV_INCLUDE_DIR)
    include_directories(SYSTEM ${LIBUV_INCLUDE_DIR})
else()
    include_directories(SYSTEM "/usr/local/include" "/opt/homebrew/include")
endif()

target_link_libraries(kraft-demo-impl-static ${LIBUV_LDFLAGS} datakit-static)

#target_link_libraries(kraft-static datakit-static)
#target_link_libraries(kraft-shared datakit-static)
#target_link_libraries(kraft-library datakit-static)

if(Build32Bit)
    set_target_properties(kraft-shared
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(kraft-static
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(kraft-library
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()
# vi:ai et sw=4 ts=4:
