# Kraft Loopy Demo - Modern Raft Server Network Platform
#
# This is a demo/example of using kraft as a consensus library.
# It is 100% isolated from kraft core - kraft knows nothing about this code.
#
# Architecture:
#   loopy-demo (this) --depends-on--> kraft (consensus library)
#   loopy-demo (this) --depends-on--> loopy (event loop)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# C11 for loopy compatibility, with GNU extensions for some platform features
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic -std=gnu11 -Wno-missing-field-initializers")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstrict-aliasing -Wstrict-overflow")

if(CMAKE_BUILD_TYPE MATCHES Debug)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2")
endif()

# Threading required
find_package(Threads REQUIRED)

# ============================================================================
# Core Module Objects
# ============================================================================
add_library(loopy-demo-impl OBJECT
    # Connection management (wraps loopyStream)
    loopyConnection.c

    # Timer management (election/heartbeat timers)
    loopyTimers.c

    # Transport layer (TCP server + outbound connections)
    loopyTransport.c

    # Session layer (handshake protocol, message framing)
    loopySession.c

    # Platform orchestration (server lifecycle, integration)
    loopyPlatform.c

    # Cluster membership and discovery
    loopyCluster.c

    # High-level server API
    loopyServer.c

    # Demo key-value state machine
    loopyKV.c

    # Client API (external TCP listener for KV commands)
    loopyClientAPI.c
)

# ============================================================================
# Static Library
# ============================================================================
add_library(loopy-demo-static STATIC $<TARGET_OBJECTS:loopy-demo-impl>)
set_target_properties(loopy-demo-static PROPERTIES PREFIX "")
set_target_properties(loopy-demo-static PROPERTIES OUTPUT_NAME loopy-demo)

# Link dependencies
target_link_libraries(loopy-demo-static
    kraft-static
    loopy-static
    datakit-static
    mbedtls
    mbedx509
    mbedcrypto
    Threads::Threads
)

# ============================================================================
# Echo Test Binary (Phase 1 verification)
# ============================================================================
add_executable(loopy-echo-test
    loopyEchoTest.c
    $<TARGET_OBJECTS:loopy-demo-impl>
)

target_link_libraries(loopy-echo-test
    kraft-static
    loopy-static
    datakit-static
    mbedtls
    mbedx509
    mbedcrypto
    Threads::Threads
)

if(APPLE)
    add_custom_command(TARGET loopy-echo-test POST_BUILD
        COMMAND dsymutil loopy-echo-test
        COMMENT "Generating OS X Debug Info for loopy-echo-test")
endif()

# ============================================================================
# Handshake Test Binary (Phase 3 verification)
# ============================================================================
add_executable(loopy-handshake-test
    loopyHandshakeTest.c
    $<TARGET_OBJECTS:loopy-demo-impl>
)

target_link_libraries(loopy-handshake-test
    kraft-static
    loopy-static
    datakit-static
    mbedtls
    mbedx509
    mbedcrypto
    Threads::Threads
)

if(APPLE)
    add_custom_command(TARGET loopy-handshake-test POST_BUILD
        COMMAND dsymutil loopy-handshake-test
        COMMENT "Generating OS X Debug Info for loopy-handshake-test")
endif()

# ============================================================================
# CLI Binary (kraft-loopy)
# ============================================================================
add_executable(kraft-loopy
    main.c
    $<TARGET_OBJECTS:loopy-demo-impl>
)

target_link_libraries(kraft-loopy
    kraft-static
    kvidxkit-static
    loopy-static
    datakit-static
    mbedtls
    mbedx509
    mbedcrypto
    Threads::Threads
)

if(APPLE)
    add_custom_command(TARGET kraft-loopy POST_BUILD
        COMMAND dsymutil kraft-loopy
        COMMENT "Generating OS X Debug Info for kraft-loopy")
endif()

# ============================================================================
# Tests
# ============================================================================
enable_testing()

add_test(NAME LoopyDemo_EchoTest
    COMMAND loopy-echo-test)
set_tests_properties(LoopyDemo_EchoTest PROPERTIES
    TIMEOUT 30
    LABELS "loopy-demo;echo;quick")

add_test(NAME LoopyDemo_HandshakeTest
    COMMAND loopy-handshake-test)
set_tests_properties(LoopyDemo_HandshakeTest PROPERTIES
    TIMEOUT 60
    LABELS "loopy-demo;handshake;protocol")

message(STATUS "Loopy Demo enabled:")
message(STATUS "  Build CLI:          make kraft-loopy")
message(STATUS "  Build echo test:    make loopy-echo-test")
message(STATUS "  Build handshake:    make loopy-handshake-test")
message(STATUS "  Run CLI:            ./kraft-loopy --listen 127.0.0.1:5001")
message(STATUS "  Run echo test:      ./loopy-echo-test")
message(STATUS "  Run handshake:      ./loopy-handshake-test")

# vi:ai et sw=4 ts=4:
