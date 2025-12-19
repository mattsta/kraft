project(kraft C)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(DK_VERSION 0.3.0)
set(DK_ABI_UPDATE 0.3.0)

if(APPLE)
    set(CMAKE_SHARED_MODULE_CREATE_C_FLAGS
        "${CMAKE_SHARED_MODULE_CREATE_C_FLAGS} -undefined dynamic_lookup")
    cmake_policy(SET CMP0042 NEW)
endif()

option(Build32Bit "Build 32-bit Library" OFF)
option(KRAFT_VERBOSE "Enable verbose debug output in kraft library" OFF)

# If you need debug, build with:
# cmake -DCMAKE_BUILD_TYPE=Debug ..
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic -std=c99 -Wno-missing-field-initializers")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstrict-aliasing -Wstrict-overflow")

if(CMAKE_BUILD_TYPE MATCHES Debug)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2")
endif()

add_library(kraft OBJECT
    # Vector library
    vec.c

    # Kraft Protocol Reader Abstraction
    protoKit.c

    # Kraft Server Connection Verification Abstraction
    serverKit.c

    # Kraft
    kraft.c
    serverAdapter.c

    # Raft Consensus RPCs
    rpc.c
    rpcFollower.c
    rpcCandidate.c
    rpcLeader.c

    # Leadership Transfer
    leadershipTransfer.c

    # ReadIndex (Linearizable Reads)
    readIndex.c

    # Metrics & Observability
    metrics.c

    # Command Batching & Pipelining
    batching.c

    # Buffer Pool (Allocation Optimization)
    bufferPool.c

    # Client Session Tracking (Exactly-Once)
    session.c

    # Witness Nodes (Read Replicas)
    witness.c

    # Async Commit Modes
    asyncCommit.c

    # Intelligent Snapshots
    snapshot.c

    # Multi-Raft Groups
    multiRaft.c

    # Configuration Profiles
    profile.c

    # Chaos Testing Framework
    chaos.c

    # CRDT-Aware Log Compaction
    crdt.c

    # Pluggable Network Infrastructure
    kraftClientAPI.c
    kraftAdapterLibuv.c
)

# Apply verbose debug output to kraft object library if enabled
# Usage: cmake -DKRAFT_VERBOSE=ON ..
if(KRAFT_VERBOSE)
    target_compile_definitions(kraft PRIVATE KRAFT_TEST_VERBOSE)
    message(STATUS "Kraft verbose debug output ENABLED")
endif()

add_library(kraft-shared  MODULE $<TARGET_OBJECTS:kraft>)
add_library(kraft-static  STATIC $<TARGET_OBJECTS:kraft>)
add_library(kraft-library SHARED $<TARGET_OBJECTS:kraft>)

set_target_properties(kraft-shared PROPERTIES PREFIX "") # don't prefix "lib"

set_target_properties(kraft-shared  PROPERTIES OUTPUT_NAME kraft)
set_target_properties(kraft-static  PROPERTIES OUTPUT_NAME kraft)
set_target_properties(kraft-library PROPERTIES OUTPUT_NAME kraft)

# SOVERSION only needs to increment when introducing *breaking* changes.
# Otherwise, just increase VERSION with normal feature additions or maint.
set_target_properties(kraft-library PROPERTIES VERSION ${DK_VERSION} SOVERSION ${DK_ABI_UPDATE})

target_link_libraries(kraft-static kvidxkit-static)
target_link_libraries(kraft-shared kvidxkit-static)
target_link_libraries(kraft-library kvidxkit-static)

target_link_libraries(kraft-static datakit-static)
target_link_libraries(kraft-shared datakit-static)
target_link_libraries(kraft-library datakit-static)

if(Build32Bit)
    set_target_properties(kraft-shared
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(kraft-static
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(kraft-library
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()

# ====================================================================
# Test Suite
# ====================================================================
option(KRAFT_BUILD_TESTS "Build Kraft test suite" ON)

if(KRAFT_BUILD_TESTS)
    add_executable(kraft-test
        kraftTest.c
        kraftTestRunner.c
        $<TARGET_OBJECTS:kraft>)

    target_compile_definitions(kraft-test PRIVATE KRAFT_TEST)

    target_link_libraries(kraft-test kvidxkit-static)
    target_link_libraries(kraft-test datakit-static)
    target_link_libraries(kraft-test m)  # math library
    target_link_libraries(kraft-test pthread)  # for any threading needs

    # Enable testing
    enable_testing()

    # Full test suite
    add_test(NAME KraftRaftTests COMMAND kraft-test)
    set_tests_properties(KraftRaftTests PROPERTIES
        TIMEOUT 120
        LABELS "raft;core")

    # Individual test cases for granular testing
    add_test(NAME Kraft_SingleNode COMMAND kraft-test 1)
    add_test(NAME Kraft_InitialElection3 COMMAND kraft-test 2)
    add_test(NAME Kraft_InitialElection5 COMMAND kraft-test 3)
    add_test(NAME Kraft_ElectionWithFailures COMMAND kraft-test 4)
    add_test(NAME Kraft_ReElection COMMAND kraft-test 5)
    add_test(NAME Kraft_ElectionSafety COMMAND kraft-test 6)
    add_test(NAME Kraft_LogMatching COMMAND kraft-test 7)
    add_test(NAME Kraft_MinorityFailures COMMAND kraft-test 8)
    add_test(NAME Kraft_NetworkPartition COMMAND kraft-test 9)
    add_test(NAME Kraft_PacketLoss COMMAND kraft-test 10)
    add_test(NAME Kraft_LeadershipTransfer COMMAND kraft-test 11)

    # Mark individual tests with appropriate labels and timeouts
    set_tests_properties(
        Kraft_SingleNode
        Kraft_InitialElection3
        Kraft_InitialElection5
        PROPERTIES
        TIMEOUT 30
        LABELS "raft;election;quick")

    set_tests_properties(
        Kraft_ElectionWithFailures
        Kraft_ReElection
        PROPERTIES
        TIMEOUT 60
        LABELS "raft;election;recovery")

    set_tests_properties(
        Kraft_ElectionSafety
        Kraft_LogMatching
        PROPERTIES
        TIMEOUT 60
        LABELS "raft;invariants")

    set_tests_properties(
        Kraft_MinorityFailures
        Kraft_NetworkPartition
        Kraft_PacketLoss
        PROPERTIES
        TIMEOUT 90
        LABELS "raft;fault-tolerance")

    set_tests_properties(
        Kraft_LeadershipTransfer
        PROPERTIES
        TIMEOUT 60
        LABELS "raft;leadership-transfer;advanced")

    # Witness Node Tests
    add_test(NAME Kraft_WitnessLifecycle COMMAND kraft-test 13)
    add_test(NAME Kraft_WitnessReplication COMMAND kraft-test 14)
    add_test(NAME Kraft_WitnessReadServing COMMAND kraft-test 15)
    add_test(NAME Kraft_WitnessPromotion COMMAND kraft-test 16)
    add_test(NAME Kraft_WitnessVotePrevention COMMAND kraft-test 17)

    set_tests_properties(
        Kraft_WitnessLifecycle
        Kraft_WitnessReplication
        Kraft_WitnessReadServing
        Kraft_WitnessPromotion
        Kraft_WitnessVotePrevention
        PROPERTIES
        TIMEOUT 30
        LABELS "raft;witness;unit")

    # Async Commit Mode Tests
    add_test(NAME Kraft_AsyncCommitConfig COMMAND kraft-test 18)
    add_test(NAME Kraft_AsyncCommitLifecycle COMMAND kraft-test 19)
    add_test(NAME Kraft_AsyncCommitStrong COMMAND kraft-test 20)
    add_test(NAME Kraft_AsyncCommitLeaderOnly COMMAND kraft-test 21)
    add_test(NAME Kraft_AsyncCommitAsync COMMAND kraft-test 22)
    add_test(NAME Kraft_AsyncCommitFlexible COMMAND kraft-test 23)
    add_test(NAME Kraft_AsyncCommitOverride COMMAND kraft-test 24)
    add_test(NAME Kraft_AsyncCommitBlocking COMMAND kraft-test 25)
    add_test(NAME Kraft_AsyncCommitStats COMMAND kraft-test 26)

    set_tests_properties(
        Kraft_AsyncCommitConfig
        Kraft_AsyncCommitLifecycle
        Kraft_AsyncCommitStrong
        Kraft_AsyncCommitLeaderOnly
        Kraft_AsyncCommitAsync
        Kraft_AsyncCommitFlexible
        Kraft_AsyncCommitOverride
        Kraft_AsyncCommitBlocking
        Kraft_AsyncCommitStats
        PROPERTIES
        TIMEOUT 30
        LABELS "raft;async-commit;unit")

    # Intelligent Snapshot Tests
    add_test(NAME Kraft_SnapshotConfig COMMAND kraft-test 27)
    add_test(NAME Kraft_SnapshotLifecycle COMMAND kraft-test 28)
    add_test(NAME Kraft_SnapshotTrigger COMMAND kraft-test 29)
    add_test(NAME Kraft_SnapshotCreation COMMAND kraft-test 30)
    add_test(NAME Kraft_SnapshotTransfer COMMAND kraft-test 31)
    add_test(NAME Kraft_SnapshotInstallation COMMAND kraft-test 32)
    add_test(NAME Kraft_SnapshotStats COMMAND kraft-test 33)

    set_tests_properties(
        Kraft_SnapshotConfig
        Kraft_SnapshotLifecycle
        Kraft_SnapshotTrigger
        Kraft_SnapshotCreation
        Kraft_SnapshotTransfer
        Kraft_SnapshotInstallation
        Kraft_SnapshotStats
        PROPERTIES
        TIMEOUT 30
        LABELS "raft;snapshot;unit")

    # Multi-Raft Group Tests
    add_test(NAME Kraft_MultiRaftConfig COMMAND kraft-test 34)
    add_test(NAME Kraft_MultiRaftLifecycle COMMAND kraft-test 35)
    add_test(NAME Kraft_MultiRaftGroupCreation COMMAND kraft-test 36)
    add_test(NAME Kraft_MultiRaftGroupRemoval COMMAND kraft-test 37)
    add_test(NAME Kraft_MultiRaftGroupStopResume COMMAND kraft-test 38)
    add_test(NAME Kraft_MultiRaftRangeLookup COMMAND kraft-test 39)
    add_test(NAME Kraft_MultiRaftTicking COMMAND kraft-test 40)
    add_test(NAME Kraft_MultiRaftStats COMMAND kraft-test 41)

    set_tests_properties(
        Kraft_MultiRaftConfig
        Kraft_MultiRaftLifecycle
        Kraft_MultiRaftGroupCreation
        Kraft_MultiRaftGroupRemoval
        Kraft_MultiRaftGroupStopResume
        Kraft_MultiRaftRangeLookup
        Kraft_MultiRaftTicking
        Kraft_MultiRaftStats
        PROPERTIES
        TIMEOUT 30
        LABELS "raft;multi-raft;unit")

    # Multi-Raft Stress/Adversarial Tests (42-54)
    add_test(NAME Kraft_MultiRaftManyGroups COMMAND kraft-test 42)
    add_test(NAME Kraft_MultiRaftHashCollisions COMMAND kraft-test 43)
    add_test(NAME Kraft_MultiRaftRangeBoundaries COMMAND kraft-test 44)
    add_test(NAME Kraft_MultiRaftSplitGroup COMMAND kraft-test 45)
    add_test(NAME Kraft_MultiRaftMergeGroups COMMAND kraft-test 46)
    add_test(NAME Kraft_MultiRaftGroupIdEnumeration COMMAND kraft-test 47)
    add_test(NAME Kraft_MultiRaftIterator COMMAND kraft-test 48)
    add_test(NAME Kraft_MultiRaftMaxCapacity COMMAND kraft-test 49)
    add_test(NAME Kraft_MultiRaftNullAndEdgeCases COMMAND kraft-test 50)
    add_test(NAME Kraft_MultiRaftConcurrentStopResume COMMAND kraft-test 51)
    add_test(NAME Kraft_MultiRaftOverlappingRanges COMMAND kraft-test 52)
    add_test(NAME Kraft_MultiRaftLongKeys COMMAND kraft-test 53)
    add_test(NAME Kraft_MultiRaftRapidCreateRemove COMMAND kraft-test 54)

    set_tests_properties(
        Kraft_MultiRaftManyGroups
        Kraft_MultiRaftHashCollisions
        Kraft_MultiRaftRangeBoundaries
        Kraft_MultiRaftSplitGroup
        Kraft_MultiRaftMergeGroups
        Kraft_MultiRaftGroupIdEnumeration
        Kraft_MultiRaftIterator
        Kraft_MultiRaftMaxCapacity
        Kraft_MultiRaftNullAndEdgeCases
        Kraft_MultiRaftConcurrentStopResume
        Kraft_MultiRaftOverlappingRanges
        Kraft_MultiRaftLongKeys
        Kraft_MultiRaftRapidCreateRemove
        PROPERTIES
        TIMEOUT 60
        LABELS "raft;multi-raft;stress")

    # Configuration Profile Tests (55-60)
    add_test(NAME Kraft_ProfileDefault COMMAND kraft-test 55)
    add_test(NAME Kraft_ProfileAllValid COMMAND kraft-test 56)
    add_test(NAME Kraft_ProfileValidation COMMAND kraft-test 57)
    add_test(NAME Kraft_ProfileOverride COMMAND kraft-test 58)
    add_test(NAME Kraft_ProfileTimingConstraints COMMAND kraft-test 59)
    add_test(NAME Kraft_ProfileNames COMMAND kraft-test 60)

    set_tests_properties(
        Kraft_ProfileDefault
        Kraft_ProfileAllValid
        Kraft_ProfileValidation
        Kraft_ProfileOverride
        Kraft_ProfileTimingConstraints
        Kraft_ProfileNames
        PROPERTIES
        TIMEOUT 10
        LABELS "raft;profile;config;unit")

    # Chaos Testing Framework Tests (61-69)
    add_test(NAME Kraft_ChaosEngineLifecycle COMMAND kraft-test 61)
    add_test(NAME Kraft_ChaosNetworkFaults COMMAND kraft-test 62)
    add_test(NAME Kraft_ChaosPartitions COMMAND kraft-test 63)
    add_test(NAME Kraft_ChaosNodeFaults COMMAND kraft-test 64)
    add_test(NAME Kraft_ChaosByzantine COMMAND kraft-test 65)
    add_test(NAME Kraft_ChaosClockManipulation COMMAND kraft-test 66)
    add_test(NAME Kraft_ChaosStats COMMAND kraft-test 67)
    add_test(NAME Kraft_ChaosPresets COMMAND kraft-test 68)
    add_test(NAME Kraft_ChaosStringHelpers COMMAND kraft-test 69)

    set_tests_properties(
        Kraft_ChaosEngineLifecycle
        Kraft_ChaosNetworkFaults
        Kraft_ChaosPartitions
        Kraft_ChaosNodeFaults
        Kraft_ChaosByzantine
        Kraft_ChaosClockManipulation
        Kraft_ChaosStats
        Kraft_ChaosPresets
        Kraft_ChaosStringHelpers
        PROPERTIES
        TIMEOUT 15
        LABELS "raft;chaos;byzantine;unit")

    # CRDT-Aware Log Compaction Tests (70-77)
    add_test(NAME Kraft_CrdtManagerLifecycle COMMAND kraft-test 70)
    add_test(NAME Kraft_CrdtKeyDeclaration COMMAND kraft-test 71)
    add_test(NAME Kraft_CrdtLwwRegister COMMAND kraft-test 72)
    add_test(NAME Kraft_CrdtGCounter COMMAND kraft-test 73)
    add_test(NAME Kraft_CrdtPnCounter COMMAND kraft-test 74)
    add_test(NAME Kraft_CrdtGSet COMMAND kraft-test 75)
    add_test(NAME Kraft_CrdtOrSet COMMAND kraft-test 76)
    add_test(NAME Kraft_CrdtStringHelpers COMMAND kraft-test 77)

    set_tests_properties(
        Kraft_CrdtManagerLifecycle
        Kraft_CrdtKeyDeclaration
        Kraft_CrdtLwwRegister
        Kraft_CrdtGCounter
        Kraft_CrdtPnCounter
        Kraft_CrdtGSet
        Kraft_CrdtOrSet
        Kraft_CrdtStringHelpers
        PROPERTIES
        TIMEOUT 15
        LABELS "raft;crdt;compaction;unit")

    message(STATUS "Kraft tests enabled - build with: make kraft-test")
    message(STATUS "Run tests with: ./kraft-test or make test")
    message(STATUS "Run specific test: ctest -R <test_name>")
    message(STATUS "Run by label: ctest -L quick")

    # Fuzzer executable
    add_executable(kraft-fuzz
        kraftTest.c
        kraftFuzz.c
        kraftFuzzRunner.c
        $<TARGET_OBJECTS:kraft>)

    target_compile_definitions(kraft-fuzz PRIVATE KRAFT_TEST)

    target_link_libraries(kraft-fuzz kvidxkit-static)
    target_link_libraries(kraft-fuzz datakit-static)
    target_link_libraries(kraft-fuzz m)
    target_link_libraries(kraft-fuzz pthread)

    # Add fuzzer tests to CTest
    add_test(NAME Kraft_Fuzz_Quick
        COMMAND kraft-fuzz -n 3 -o 500 -t 10)
    set_tests_properties(Kraft_Fuzz_Quick PROPERTIES
        TIMEOUT 30
        LABELS "raft;fuzz;quick")

    add_test(NAME Kraft_Fuzz_Standard
        COMMAND kraft-fuzz -n 5 -o 1000 -t 30)
    set_tests_properties(Kraft_Fuzz_Standard PROPERTIES
        TIMEOUT 60
        LABELS "raft;fuzz;standard")

    add_test(NAME Kraft_Fuzz_Stress
        COMMAND kraft-fuzz -n 7 -o 5000 -t 120)
    set_tests_properties(Kraft_Fuzz_Stress PROPERTIES
        TIMEOUT 180
        LABELS "raft;fuzz;stress")

    message(STATUS "Kraft fuzzer enabled - build with: make kraft-fuzz")
    message(STATUS "Run fuzzer with: ./kraft-fuzz [options]")
    message(STATUS "Run quick fuzz: ctest -R Kraft_Fuzz_Quick")
    message(STATUS "Run all fuzz tests: ctest -L fuzz")

    # Benchmark executable
    add_executable(kraft-bench
        kraftTest.c
        kraftBench.c
        $<TARGET_OBJECTS:kraft>)

    target_compile_definitions(kraft-bench PRIVATE KRAFT_TEST)

    target_link_libraries(kraft-bench kvidxkit-static)
    target_link_libraries(kraft-bench datakit-static)
    target_link_libraries(kraft-bench m)
    target_link_libraries(kraft-bench pthread)

    message(STATUS "Kraft benchmark enabled - build with: make kraft-bench")
    message(STATUS "Run all benchmarks: ./kraft-bench")
    message(STATUS "Run specific benchmark: ./kraft-bench <throughput|latency|election|scalability|fault-tolerance>")

    # ====================================================================
    # Multi-Raft Group Fuzzer
    # ====================================================================
    add_executable(kraft-fuzz-multi
        kraftTest.c
        kraftFuzzMulti.c
        kraftFuzzMultiRunner.c
        $<TARGET_OBJECTS:kraft>)

    target_compile_definitions(kraft-fuzz-multi PRIVATE KRAFT_TEST)

    target_link_libraries(kraft-fuzz-multi kvidxkit-static)
    target_link_libraries(kraft-fuzz-multi datakit-static)
    target_link_libraries(kraft-fuzz-multi m)
    target_link_libraries(kraft-fuzz-multi pthread)

    # Add Multi-Raft fuzzer tests to CTest
    # -g = number of groups, -n = nodes per group, -o = max ops, -t = max time (sec)
    add_test(NAME Kraft_FuzzMulti_Quick
        COMMAND kraft-fuzz-multi -g 2 -n 3 -o 1000 -t 15)
    set_tests_properties(Kraft_FuzzMulti_Quick PROPERTIES
        TIMEOUT 30
        LABELS "raft;multi-raft;fuzz;quick")

    add_test(NAME Kraft_FuzzMulti_Standard
        COMMAND kraft-fuzz-multi -g 3 -n 5 -o 5000 -t 30)
    set_tests_properties(Kraft_FuzzMulti_Standard PROPERTIES
        TIMEOUT 60
        LABELS "raft;multi-raft;fuzz;standard")

    add_test(NAME Kraft_FuzzMulti_Stress
        COMMAND kraft-fuzz-multi -g 5 -n 5 -o 20000 -t 90)
    set_tests_properties(Kraft_FuzzMulti_Stress PROPERTIES
        TIMEOUT 180
        LABELS "raft;multi-raft;fuzz;stress")

    message(STATUS "Multi-Raft fuzzer enabled - build with: make kraft-fuzz-multi")
    message(STATUS "Run Multi-Raft fuzzer with: ./kraft-fuzz-multi [options]")

    # ====================================================================
    # Feature Combination Fuzzer
    # ====================================================================
    add_executable(kraft-fuzz-combo
        kraftTest.c
        kraftFuzzCombo.c
        kraftFuzzComboRunner.c
        $<TARGET_OBJECTS:kraft>)

    target_compile_definitions(kraft-fuzz-combo PRIVATE KRAFT_TEST)

    target_link_libraries(kraft-fuzz-combo kvidxkit-static)
    target_link_libraries(kraft-fuzz-combo datakit-static)
    target_link_libraries(kraft-fuzz-combo m)
    target_link_libraries(kraft-fuzz-combo pthread)

    # Add Combo fuzzer tests to CTest
    add_test(NAME Kraft_FuzzCombo_Quick
        COMMAND kraft-fuzz-combo -c 4 -o 200 -t 5)
    set_tests_properties(Kraft_FuzzCombo_Quick PROPERTIES
        TIMEOUT 60
        LABELS "raft;combo;fuzz;quick")

    add_test(NAME Kraft_FuzzCombo_Standard
        COMMAND kraft-fuzz-combo -c 16 -o 500 -t 10)
    set_tests_properties(Kraft_FuzzCombo_Standard PROPERTIES
        TIMEOUT 300
        LABELS "raft;combo;fuzz;standard")

    add_test(NAME Kraft_FuzzCombo_Comprehensive
        COMMAND kraft-fuzz-combo -c 64 -o 1000 -t 30)
    set_tests_properties(Kraft_FuzzCombo_Comprehensive PROPERTIES
        TIMEOUT 600
        LABELS "raft;combo;fuzz;stress")

    message(STATUS "Combo fuzzer enabled - build with: make kraft-fuzz-combo")
    message(STATUS "Run Combo fuzzer with: ./kraft-fuzz-combo [options]")
    message(STATUS "Run all fuzz tests: ctest -L fuzz")

    # ====================================================================
    # Chaos-Enhanced Fuzzer
    # ====================================================================
    add_executable(kraft-fuzz-chaos
        kraftTest.c
        kraftFuzzChaos.c
        kraftFuzzChaosRunner.c
        $<TARGET_OBJECTS:kraft>)

    target_compile_definitions(kraft-fuzz-chaos PRIVATE KRAFT_TEST)

    target_link_libraries(kraft-fuzz-chaos kvidxkit-static)
    target_link_libraries(kraft-fuzz-chaos datakit-static)
    target_link_libraries(kraft-fuzz-chaos m)
    target_link_libraries(kraft-fuzz-chaos pthread)

    # Chaos fuzzer tests
    add_test(NAME Kraft_FuzzChaos_Quick
        COMMAND kraft-fuzz-chaos --scenario mixed -o 1000 -t 10)
    set_tests_properties(Kraft_FuzzChaos_Quick PROPERTIES
        TIMEOUT 30
        LABELS "raft;chaos;fuzz;quick")

    add_test(NAME Kraft_FuzzChaos_Storm
        COMMAND kraft-fuzz-chaos --scenario storm -o 3000 -t 30)
    set_tests_properties(Kraft_FuzzChaos_Storm PROPERTIES
        TIMEOUT 60
        LABELS "raft;chaos;fuzz;partition")

    add_test(NAME Kraft_FuzzChaos_Byzantine
        COMMAND kraft-fuzz-chaos --scenario byzantine -o 3000 -t 30)
    set_tests_properties(Kraft_FuzzChaos_Byzantine PROPERTIES
        TIMEOUT 60
        LABELS "raft;chaos;fuzz;byzantine")

    add_test(NAME Kraft_FuzzChaos_Hellweek
        COMMAND kraft-fuzz-chaos --scenario hellweek -o 5000 -t 60)
    set_tests_properties(Kraft_FuzzChaos_Hellweek PROPERTIES
        TIMEOUT 120
        LABELS "raft;chaos;fuzz;stress")

    message(STATUS "Chaos fuzzer enabled - build with: make kraft-fuzz-chaos")
    message(STATUS "Run chaos fuzzer: ./kraft-fuzz-chaos [--scenario NAME]")
    message(STATUS "Scenarios: flaky, storm, byzantine, hellweek, mixed")

    # ====================================================================
    # Linearizability Testing Framework
    # ====================================================================
    add_executable(kraft-linearizability
        kraftLinearizability.c
        kraftLinearizabilityChecker.c
        kraftLinearizabilityCluster.c
        kraftLinearizabilityWorkload.c
        kraftLinearizabilityFailure.c
        kraftLinearizabilityRunner.c)

    target_compile_definitions(kraft-linearizability PRIVATE KRAFT_TEST)

    target_link_libraries(kraft-linearizability m)
    target_link_libraries(kraft-linearizability pthread)

    # Linearizability tests
    add_test(NAME Kraft_Linearizability_Quick
        COMMAND kraft-linearizability --preset quick)
    set_tests_properties(Kraft_Linearizability_Quick PROPERTIES
        TIMEOUT 60
        LABELS "raft;linearizability;correctness;quick")

    add_test(NAME Kraft_Linearizability_Standard
        COMMAND kraft-linearizability --preset standard)
    set_tests_properties(Kraft_Linearizability_Standard PROPERTIES
        TIMEOUT 360
        LABELS "raft;linearizability;correctness;standard")

    add_test(NAME Kraft_Linearizability_CI
        COMMAND kraft-linearizability --preset ci)
    set_tests_properties(Kraft_Linearizability_CI PROPERTIES
        TIMEOUT 180
        LABELS "raft;linearizability;correctness;ci")

    message(STATUS "Linearizability tester enabled - build with: make kraft-linearizability")
    message(STATUS "Run linearizability tests: ./kraft-linearizability --preset quick")
    message(STATUS "CTest integration: ctest -L linearizability")

    # ====================================================================
    # CRDT Property Fuzzer
    # ====================================================================
    add_executable(kraft-fuzz-crdt
        kraftFuzzCrdt.c
        kraftFuzzCrdtRunner.c)

    target_compile_definitions(kraft-fuzz-crdt PRIVATE KRAFT_TEST)

    target_link_libraries(kraft-fuzz-crdt m)

    # CRDT property tests
    add_test(NAME Kraft_FuzzCrdt_Quick
        COMMAND kraft-fuzz-crdt -o 50)
    set_tests_properties(Kraft_FuzzCrdt_Quick PROPERTIES
        TIMEOUT 15
        LABELS "raft;crdt;fuzz;quick")

    add_test(NAME Kraft_FuzzCrdt_Standard
        COMMAND kraft-fuzz-crdt -o 200)
    set_tests_properties(Kraft_FuzzCrdt_Standard PROPERTIES
        TIMEOUT 30
        LABELS "raft;crdt;fuzz;standard")

    add_test(NAME Kraft_FuzzCrdt_Thorough
        COMMAND kraft-fuzz-crdt -o 1000 -r 10)
    set_tests_properties(Kraft_FuzzCrdt_Thorough PROPERTIES
        TIMEOUT 120
        LABELS "raft;crdt;fuzz;stress")

    message(STATUS "CRDT fuzzer enabled - build with: make kraft-fuzz-crdt")
    message(STATUS "Run CRDT fuzzer: ./kraft-fuzz-crdt [--type TYPE] [--property PROP]")
endif()

# vi:ai et sw=4 ts=4:
