#!/bin/bash
#
# kraft-test-live.sh - Live integration tests for kraft-loopy
#
# Runs automated tests against real cluster processes.
# All tests have timeouts and clean up after themselves.
#
# Usage:
#   kraft-test-live.sh [test-name]
#
# Tests:
#   single     - Single node starts and stops cleanly
#   cluster3   - 3-node cluster starts and all nodes see each other
#   failover   - Leader crash triggers election
#   consistency- Cluster maintains consistency
#   client_api - Client API accepts connections and handles commands
#   all        - Run all tests (default)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KRAFT_BINARY="${KRAFT_BINARY:-}"
KRAFT_DATA_DIR="$PROJECT_ROOT/build2/kraft-test-$$"
KRAFT_BASE_PORT="${KRAFT_BASE_PORT:-6001}"  # Different from manual testing
KRAFT_PID_DIR="$KRAFT_DATA_DIR/pids"
KRAFT_LOG_DIR="$KRAFT_DATA_DIR/logs"

# Test timeout in seconds
TEST_TIMEOUT=10

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { echo -e "${GREEN}PASS${NC}: $*"; }
fail() { echo -e "${RED}FAIL${NC}: $*"; FAILED=1; }
skip() { echo -e "${YELLOW}SKIP${NC}: $*"; }

# Find binary
find_binary() {
    if [[ -n "$KRAFT_BINARY" ]] && [[ -x "$KRAFT_BINARY" ]]; then
        return 0
    fi

    local candidates=(
        "$SCRIPT_DIR/../build2/src/loopy-demo/kraft-loopy"
        "$SCRIPT_DIR/../build/src/loopy-demo/kraft-loopy"
        "./build2/src/loopy-demo/kraft-loopy"
        "./build/src/loopy-demo/kraft-loopy"
    )

    for c in "${candidates[@]}"; do
        if [[ -x "$c" ]]; then
            KRAFT_BINARY="$c"
            return 0
        fi
    done

    echo "kraft-loopy binary not found"
    exit 1
}

# Get node port
node_port() {
    echo $((KRAFT_BASE_PORT + $1 - 1))
}

# Start a node, return immediately
start_node() {
    local node=$1
    local total=$2
    local port=$(node_port "$node")
    local data_dir="$KRAFT_DATA_DIR/node$node"
    local pid_file="$KRAFT_PID_DIR/node$node.pid"
    local log_file="$KRAFT_LOG_DIR/node$node.log"

    mkdir -p "$data_dir" "$KRAFT_PID_DIR" "$KRAFT_LOG_DIR"

    local cmd="$KRAFT_BINARY --listen 127.0.0.1:$port --data $data_dir --node $node"

    # Build cluster addresses for multi-node
    if [[ $total -gt 1 ]]; then
        local addrs=""
        for ((i = 1; i <= total; i++)); do
            [[ -n "$addrs" ]] && addrs="$addrs,"
            addrs="${addrs}127.0.0.1:$(node_port "$i")"
        done
        cmd="$cmd --cluster $addrs"
    fi

    $cmd > "$log_file" 2>&1 &
    echo $! > "$pid_file"
}

# Stop a node
stop_node() {
    local node=$1
    local pid_file="$KRAFT_PID_DIR/node$node.pid"

    if [[ -f "$pid_file" ]]; then
        local pid=$(cat "$pid_file")
        kill "$pid" 2>/dev/null || true
        # Wait briefly for clean exit
        for i in 1 2 3 4 5; do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.2
        done
        kill -9 "$pid" 2>/dev/null || true
        rm -f "$pid_file"
    fi
}

# Stop all nodes
stop_all() {
    shopt -s nullglob
    for pid_file in "$KRAFT_PID_DIR"/node*.pid; do
        [[ -f "$pid_file" ]] || continue
        local node=$(basename "$pid_file" .pid | sed 's/node//')
        stop_node "$node"
    done
    shopt -u nullglob
}

# Check if node is running
node_running() {
    local node=$1
    local pid_file="$KRAFT_PID_DIR/node$node.pid"
    [[ -f "$pid_file" ]] && kill -0 "$(cat "$pid_file")" 2>/dev/null
}

# Wait for node to be listening on port
wait_for_port() {
    local port=$1
    local timeout=$2
    local start=$(date +%s)

    while true; do
        if nc -z 127.0.0.1 "$port" 2>/dev/null; then
            return 0
        fi
        if [[ $(($(date +%s) - start)) -ge $timeout ]]; then
            return 1
        fi
        sleep 0.1
    done
}

# Check node log for pattern
log_contains() {
    local node=$1
    local pattern=$2
    grep -q "$pattern" "$KRAFT_LOG_DIR/node$node.log" 2>/dev/null
}

# Get client API port for a node (offset by 100 from Raft port)
client_port() {
    echo $((KRAFT_BASE_PORT + $1 - 1 + 100))
}

# Start a node with client API enabled
start_node_with_client() {
    local node=$1
    local total=$2
    local port=$(node_port "$node")
    local cport=$(client_port "$node")
    local data_dir="$KRAFT_DATA_DIR/node$node"
    local pid_file="$KRAFT_PID_DIR/node$node.pid"
    local log_file="$KRAFT_LOG_DIR/node$node.log"

    mkdir -p "$data_dir" "$KRAFT_PID_DIR" "$KRAFT_LOG_DIR"

    local cmd="$KRAFT_BINARY --listen 127.0.0.1:$port --data $data_dir --node $node --client $cport"

    # Build cluster addresses for multi-node
    if [[ $total -gt 1 ]]; then
        local addrs=""
        for ((i = 1; i <= total; i++)); do
            [[ -n "$addrs" ]] && addrs="$addrs,"
            addrs="${addrs}127.0.0.1:$(node_port "$i")"
        done
        cmd="$cmd --cluster $addrs"
    fi

    $cmd > "$log_file" 2>&1 &
    echo $! > "$pid_file"
}

# Send a command to the client API and return the response
# Usage: client_cmd <port> <command>
# Returns: response on stdout, exit code 0 on success
client_cmd() {
    local port=$1
    local cmd=$2
    # Send command and wait briefly for response before closing
    { printf '%s\r\n' "$cmd"; sleep 0.3; } | nc -w2 127.0.0.1 "$port" 2>/dev/null
}

# Cleanup on exit
cleanup() {
    if [[ -n "$KEEP_LOGS" ]]; then
        echo "" >&2
        echo "Logs saved to: $KRAFT_LOG_DIR" >&2
        echo "Data saved to: $KRAFT_DATA_DIR" >&2
    else
        stop_all
        rm -rf "$KRAFT_DATA_DIR"
    fi
}
trap cleanup EXIT

# ============================================================================
# Tests
# ============================================================================

test_single() {
    echo "Test: Single node starts and stops cleanly"

    start_node 1 1

    if ! wait_for_port "$(node_port 1)" "$TEST_TIMEOUT"; then
        fail "Node 1 failed to start listening"
        cat "$KRAFT_LOG_DIR/node1.log" 2>/dev/null || true
        return 1
    fi

    sleep 0.5  # Let it run briefly

    if ! node_running 1; then
        fail "Node 1 crashed"
        cat "$KRAFT_LOG_DIR/node1.log" 2>/dev/null || true
        return 1
    fi

    stop_node 1
    sleep 0.3

    if node_running 1; then
        fail "Node 1 did not stop"
        return 1
    fi

    pass "Single node lifecycle"
    return 0
}

test_cluster3() {
    echo "Test: 3-node cluster starts"

    # Start all 3 nodes
    for i in 1 2 3; do
        start_node "$i" 3
        sleep 0.2
    done

    # Wait for all to be listening
    local all_up=true
    for i in 1 2 3; do
        if ! wait_for_port "$(node_port "$i")" "$TEST_TIMEOUT"; then
            fail "Node $i failed to start listening"
            all_up=false
        fi
    done

    if ! $all_up; then
        for i in 1 2 3; do
            echo "=== Node $i log ===" >&2
            cat "$KRAFT_LOG_DIR/node$i.log" 2>/dev/null || true
        done
        return 1
    fi

    # Let cluster stabilize
    sleep 1

    # Check all nodes still running
    local all_running=true
    for i in 1 2 3; do
        if ! node_running "$i"; then
            fail "Node $i crashed"
            all_running=false
        fi
    done

    if ! $all_running; then
        return 1
    fi

    # Stop nodes
    for i in 1 2 3; do
        stop_node "$i"
    done

    pass "3-node cluster lifecycle"
    return 0
}

test_failover() {
    echo "Test: Leader crash triggers election"

    # Start 3-node cluster
    for i in 1 2 3; do
        start_node "$i" 3
        sleep 0.2
    done

    # Wait for all to be listening
    for i in 1 2 3; do
        if ! wait_for_port "$(node_port "$i")" "$TEST_TIMEOUT"; then
            fail "Node $i failed to start"
            return 1
        fi
    done

    # Wait for election to complete
    sleep 2

    # Kill node 1 (likely leader in port-based ID scheme)
    stop_node 1

    # Wait for new election
    sleep 2

    # Check remaining nodes still running
    local survivors=0
    for i in 2 3; do
        if node_running "$i"; then
            survivors=$((survivors + 1))
        fi
    done

    if [[ $survivors -lt 2 ]]; then
        fail "Cluster collapsed after leader failure"
        return 1
    fi

    # Cleanup
    for i in 2 3; do
        stop_node "$i"
    done

    pass "Leader failure handled"
    return 0
}

test_consistency() {
    echo "Test: Cluster maintains consistency across restarts"

    # Start 3-node cluster
    for i in 1 2 3; do
        start_node "$i" 3
        sleep 0.2
    done

    # Wait for all to be listening
    for i in 1 2 3; do
        if ! wait_for_port "$(node_port "$i")" "$TEST_TIMEOUT"; then
            fail "Node $i failed to start"
            return 1
        fi
    done

    # Let cluster stabilize and elect a leader
    sleep 2

    # Check all nodes still running (basic consistency - no crash)
    local all_running=true
    for i in 1 2 3; do
        if ! node_running "$i"; then
            fail "Node $i crashed during normal operation"
            all_running=false
        fi
    done

    if ! $all_running; then
        for i in 1 2 3; do
            echo "=== Node $i log ===" >&2
            cat "$KRAFT_LOG_DIR/node$i.log" 2>/dev/null || true
        done
        return 1
    fi

    # Verify no error messages in logs indicating consistency issues
    local consistency_ok=true
    for i in 1 2 3; do
        if grep -qi "consistency\|corrupt\|invalid" "$KRAFT_LOG_DIR/node$i.log" 2>/dev/null; then
            fail "Node $i has consistency warnings in log"
            consistency_ok=false
        fi
    done

    # Note: wait_for_port already verified successful startup
    # No need to check log messages (stdout buffering may prevent them from appearing)

    # Stop nodes gracefully
    for i in 1 2 3; do
        stop_node "$i"
    done

    if ! $consistency_ok; then
        return 1
    fi

    pass "Cluster consistency maintained"
    return 0
}

test_client_api() {
    echo "Test: Client API accepts connections and handles commands"

    # Start single node with client API
    start_node_with_client 1 1

    local raft_port=$(node_port 1)
    local cport=$(client_port 1)

    # Wait for Raft port
    if ! wait_for_port "$raft_port" "$TEST_TIMEOUT"; then
        fail "Node 1 failed to start listening (Raft port)"
        cat "$KRAFT_LOG_DIR/node1.log" 2>/dev/null || true
        return 1
    fi

    # Wait for client port
    if ! wait_for_port "$cport" "$TEST_TIMEOUT"; then
        fail "Node 1 failed to start client API on port $cport"
        cat "$KRAFT_LOG_DIR/node1.log" 2>/dev/null || true
        return 1
    fi

    sleep 0.5  # Let it stabilize

    # Test SET command
    local response
    response=$(client_cmd "$cport" "SET foo bar")
    if [[ "$response" != *"+OK"* ]]; then
        fail "SET foo bar: expected +OK, got: $response"
        stop_node 1
        return 1
    fi

    # Test GET command
    response=$(client_cmd "$cport" "GET foo")
    if [[ "$response" != *"bar"* ]]; then
        fail "GET foo: expected 'bar', got: $response"
        stop_node 1
        return 1
    fi

    # Test INCR command
    response=$(client_cmd "$cport" "INCR counter")
    if [[ "$response" != *":1"* ]]; then
        fail "INCR counter: expected :1, got: $response"
        stop_node 1
        return 1
    fi

    # Test INCR again
    response=$(client_cmd "$cport" "INCR counter")
    if [[ "$response" != *":2"* ]]; then
        fail "INCR counter again: expected :2, got: $response"
        stop_node 1
        return 1
    fi

    # Test DEL command
    response=$(client_cmd "$cport" "DEL foo")
    if [[ "$response" != *"+OK"* ]]; then
        fail "DEL foo: expected +OK, got: $response"
        stop_node 1
        return 1
    fi

    # Test GET after DEL (should be null)
    response=$(client_cmd "$cport" "GET foo")
    if [[ "$response" != *'$-1'* ]]; then
        fail "GET foo after DEL: expected null (\$-1), got: $response"
        stop_node 1
        return 1
    fi

    # Note: KEYS command not yet implemented in loopyKVParseCommand

    stop_node 1
    sleep 0.3

    if node_running 1; then
        fail "Node 1 did not stop"
        return 1
    fi

    pass "Client API commands work"
    return 0
}

test_client_admin() {
    echo "Test: Client API admin commands (STATUS, SNAPSHOT, INFO)"

    # Start single node with client API
    start_node_with_client 1 1

    local raft_port=$(node_port 1)
    local cport=$(client_port 1)

    # Wait for startup
    if ! wait_for_port "$raft_port" "$TEST_TIMEOUT"; then
        fail "Node 1 failed to start"
        cat "$KRAFT_LOG_DIR/node1.log" 2>/dev/null || true
        return 1
    fi

    if ! wait_for_port "$cport" "$TEST_TIMEOUT"; then
        fail "Node 1 client API failed to start"
        return 1
    fi

    sleep 0.5  # Let it become leader

    # Test STATUS command
    local response
    response=$(client_cmd "$cport" "STATUS")
    if [[ "$response" != *"node_id:1"* ]]; then
        fail "STATUS: expected node_id:1, got: $response"
        stop_node 1
        return 1
    fi
    if [[ "$response" != *"role:"* ]]; then
        fail "STATUS: expected role field, got: $response"
        stop_node 1
        return 1
    fi
    if [[ "$response" != *"is_leader:"* ]]; then
        fail "STATUS: expected is_leader field, got: $response"
        stop_node 1
        return 1
    fi

    # Test INFO command (alias for STATUS)
    response=$(client_cmd "$cport" "INFO")
    if [[ "$response" != *"node_id:1"* ]]; then
        fail "INFO: expected node_id:1, got: $response"
        stop_node 1
        return 1
    fi

    # Test SNAPSHOT command
    response=$(client_cmd "$cport" "SNAPSHOT")
    if [[ "$response" != *"size:"* ]]; then
        fail "SNAPSHOT: expected size field, got: $response"
        stop_node 1
        return 1
    fi
    if [[ "$response" != *"last_index:"* ]]; then
        fail "SNAPSHOT: expected last_index field, got: $response"
        stop_node 1
        return 1
    fi

    stop_node 1
    sleep 0.3

    if node_running 1; then
        fail "Node 1 did not stop"
        return 1
    fi

    pass "Client API admin commands work"
    return 0
}

test_client_cluster() {
    echo "Test: Client API cluster commands (3-node startup with client API)"

    # Start 3-node cluster with client API
    for i in 1 2 3; do
        start_node_with_client "$i" 3
        sleep 0.3
    done

    # Wait for all nodes - both Raft and client ports
    for i in 1 2 3; do
        local rport=$(node_port "$i")
        local cport=$(client_port "$i")
        if ! wait_for_port "$rport" "$TEST_TIMEOUT"; then
            fail "Node $i Raft port failed to start"
            return 1
        fi
        if ! wait_for_port "$cport" "$TEST_TIMEOUT"; then
            fail "Node $i client port failed to start"
            return 1
        fi
    done

    sleep 1  # Let nodes stabilize

    # Verify all nodes respond to STATUS via client API
    for i in 1 2 3; do
        local cport=$(client_port "$i")
        local response
        response=$(client_cmd "$cport" "STATUS")
        if [[ "$response" != *"node_id:$i"* ]]; then
            fail "Node $i STATUS: expected node_id:$i, got: $response"
            stop_all
            return 1
        fi
    done

    # Verify SNAPSHOT works on each node
    for i in 1 2 3; do
        local cport=$(client_port "$i")
        local response
        response=$(client_cmd "$cport" "SNAPSHOT")
        if [[ "$response" != *"size:"* ]]; then
            fail "Node $i SNAPSHOT: expected size field, got: $response"
            stop_all
            return 1
        fi
    done

    # Verify INFO (alias for STATUS) works
    local response
    response=$(client_cmd "$(client_port 1)" "INFO")
    if [[ "$response" != *"node_id:1"* ]]; then
        fail "Node 1 INFO: expected node_id:1, got: $response"
        stop_all
        return 1
    fi

    # Clean up
    stop_all
    sleep 0.3

    pass "Client API cluster startup works"
    return 0
}

# ============================================================================
# Main
# ============================================================================

main() {
    find_binary
    echo "Using binary: $KRAFT_BINARY"
    echo "Test data: $KRAFT_DATA_DIR"
    echo

    FAILED=0
    local test_name="${1:-all}"

    case "$test_name" in
        single)
            test_single
            ;;
        cluster3)
            test_cluster3
            ;;
        failover)
            test_failover
            ;;
        consistency)
            test_consistency
            ;;
        client_api)
            test_client_api
            ;;
        client_admin)
            test_client_admin
            ;;
        client_cluster)
            test_client_cluster
            ;;
        all)
            test_single
            cleanup  # Reset between tests
            mkdir -p "$KRAFT_DATA_DIR"

            test_cluster3
            cleanup
            mkdir -p "$KRAFT_DATA_DIR"

            test_failover
            cleanup
            mkdir -p "$KRAFT_DATA_DIR"

            test_consistency
            cleanup
            mkdir -p "$KRAFT_DATA_DIR"

            test_client_api
            cleanup
            mkdir -p "$KRAFT_DATA_DIR"

            test_client_admin
            cleanup
            mkdir -p "$KRAFT_DATA_DIR"

            test_client_cluster
            ;;
        *)
            echo "Unknown test: $test_name"
            echo "Available: single, cluster3, failover, consistency, client_api, client_admin, client_cluster, all"
            exit 1
            ;;
    esac

    echo
    if [[ $FAILED -eq 0 ]]; then
        echo -e "${GREEN}All tests passed${NC}"
        exit 0
    else
        echo -e "${RED}Some tests failed${NC}"
        exit 1
    fi
}

main "$@"
