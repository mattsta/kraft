#!/bin/bash
#
# kraft-test-linearizability.sh - Comprehensive Linearizability Test Harness
#
# Tests linearizability guarantees across cluster sizes from 3 to 69 nodes,
# including failure injection scenarios.
#
# Usage:
#   ./scripts/kraft-test-linearizability.sh [OPTIONS]
#
# Options:
#   --quick           Run quick tests (3,5,7 nodes only)
#   --standard        Run standard tests (3,5,7,11,15 nodes)
#   --full            Run full tests (3-27 nodes)
#   --stress          Run stress tests (3-69 nodes)
#   --failures        Include failure injection tests
#   --all-failures    Test all failure modes
#   --verbose         Verbose output
#   --ci              CI mode (exit on first failure)
#   --keep-logs       Keep logs even on success
#
# Environment:
#   KRAFT_LOOPY_BINARY    Path to kraft-loopy binary
#   KRAFT_LIN_BINARY      Path to kraft-linearizability binary
#   KRAFT_BASE_PORT       Base port for tests (default: 10000)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Binaries
KRAFT_LOOPY="${KRAFT_LOOPY_BINARY:-$PROJECT_ROOT/build2/src/loopy-demo/kraft-loopy}"
KRAFT_LIN="${KRAFT_LIN_BINARY:-$PROJECT_ROOT/build2/src/kraft-linearizability}"

# Export for use by kraft-linearizability
export KRAFT_LOOPY_BINARY="$KRAFT_LOOPY"

# Default settings
VERBOSE=false
CI_MODE=false
KEEP_LOGS=false
TEST_FAILURES=false
ALL_FAILURES=false
TEST_PROFILE="standard"
BASE_PORT="${KRAFT_BASE_PORT:-10000}"

# Test presets
QUICK_SIZES=(3 5 7)
STANDARD_SIZES=(3 5 7 11 15)
FULL_SIZES=(3 5 7 9 11 15 21 27)
STRESS_SIZES=(3 5 7 9 11 15 21 27 33 45 51 69)

# Failure modes to test
FAILURE_MODES=("crash-leader" "crash-follower" "partition-leader" "partition-minority")

# Results tracking
PASSED=0
FAILED=0
SKIPPED=0
declare -a FAILURES

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --quick)
            TEST_PROFILE="quick"
            shift
            ;;
        --standard)
            TEST_PROFILE="standard"
            shift
            ;;
        --full)
            TEST_PROFILE="full"
            shift
            ;;
        --stress)
            TEST_PROFILE="stress"
            shift
            ;;
        --failures)
            TEST_FAILURES=true
            shift
            ;;
        --all-failures)
            TEST_FAILURES=true
            ALL_FAILURES=true
            shift
            ;;
        --verbose)
            VERBOSE=true
            shift
            ;;
        --ci)
            CI_MODE=true
            shift
            ;;
        --keep-logs)
            KEEP_LOGS=true
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --quick         Quick tests (3,5,7 nodes)"
            echo "  --standard      Standard tests (3,5,7,11,15 nodes)"
            echo "  --full          Full tests (3-27 nodes)"
            echo "  --stress        Stress tests (3-69 nodes)"
            echo "  --failures      Include failure injection tests"
            echo "  --all-failures  Test all failure modes"
            echo "  --verbose       Verbose output"
            echo "  --ci            CI mode (exit on first failure)"
            echo "  --keep-logs     Keep logs even on success"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Select test sizes based on profile
case $TEST_PROFILE in
    quick)
        SIZES=("${QUICK_SIZES[@]}")
        ;;
    standard)
        SIZES=("${STANDARD_SIZES[@]}")
        ;;
    full)
        SIZES=("${FULL_SIZES[@]}")
        ;;
    stress)
        SIZES=("${STRESS_SIZES[@]}")
        ;;
esac

# Setup results directory
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
RESULTS_DIR="$PROJECT_ROOT/build2/linearizability-tests-$TIMESTAMP"
mkdir -p "$RESULTS_DIR"

log() {
    echo -e "${BLUE}[$(date +%H:%M:%S)]${NC} $*"
}

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $*"
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $*"
}

log_skip() {
    echo -e "${YELLOW}[SKIP]${NC} $*"
}

# Check prerequisites
check_prerequisites() {
    log "Checking prerequisites..."

    if [[ ! -x "$KRAFT_LOOPY" ]]; then
        echo "ERROR: kraft-loopy binary not found at: $KRAFT_LOOPY"
        echo "Build with: cd build2 && cmake .. && make"
        exit 1
    fi

    if [[ ! -x "$KRAFT_LIN" ]]; then
        echo "ERROR: kraft-linearizability binary not found at: $KRAFT_LIN"
        echo "Build with: cd build2 && cmake .. && make"
        exit 1
    fi

    log "  kraft-loopy: $KRAFT_LOOPY"
    log "  kraft-linearizability: $KRAFT_LIN"
    log "  Results: $RESULTS_DIR"
}

# Calculate test parameters based on cluster size
get_test_params() {
    local size=$1
    local ops clients timeout

    if [[ $size -le 7 ]]; then
        ops=500
        clients=5
        timeout=60
    elif [[ $size -le 15 ]]; then
        ops=300
        clients=4
        timeout=90
    elif [[ $size -le 27 ]]; then
        ops=200
        clients=3
        timeout=120
    else
        ops=100
        clients=2
        timeout=180
    fi

    echo "$ops $clients $timeout"
}

# Run a single linearizability test
run_linearizability_test() {
    local size=$1
    local failure_mode=${2:-none}
    local test_name="lin-${size}n"

    if [[ "$failure_mode" != "none" ]]; then
        test_name="${test_name}-${failure_mode}"
    fi

    local test_log="$RESULTS_DIR/${test_name}.log"
    local test_json="$RESULTS_DIR/${test_name}.json"

    read -r ops clients timeout <<< "$(get_test_params $size)"

    # Use unique data directory for each test to avoid conflicts
    local test_data_dir="$RESULTS_DIR/data-${test_name}"
    mkdir -p "$test_data_dir"

    log "Test: $test_name (${size} nodes, ${ops} ops, ${clients} clients)"

    local cmd="$KRAFT_LIN -n $size -c $clients -o $ops -t ${timeout}s --data-dir $test_data_dir"

    if [[ "$failure_mode" != "none" ]]; then
        cmd="$cmd --failure $failure_mode"
    fi

    if $VERBOSE; then
        cmd="$cmd --verbose"
    fi

    cmd="$cmd --json $test_json"

    if $VERBOSE; then
        log "  Command: $cmd"
    fi

    # Run with timeout (add extra 30s buffer)
    local total_timeout=$((timeout + 60))

    if timeout $total_timeout $cmd > "$test_log" 2>&1; then
        # Check result
        if grep -q "RESULT: PASS" "$test_log" 2>/dev/null; then
            log_pass "$test_name"
            PASSED=$((PASSED + 1))

            # Extract stats if verbose
            if $VERBOSE; then
                local completions=$(grep "Completions:" "$test_log" | awk '{print $2}' || echo "?")
                local timeouts=$(grep "Timeouts:" "$test_log" | awk '{print $2}' || echo "?")
                log "  Completions: $completions, Timeouts: $timeouts"
            fi

            return 0
        else
            log_fail "$test_name - Linearizability violation detected"
            FAILED=$((FAILED + 1))
            FAILURES+=("$test_name")

            # Show violation details
            if grep -q "VIOLATION" "$test_log" 2>/dev/null; then
                echo "  Violation details:"
                grep -A 10 "VIOLATION" "$test_log" | head -15 | sed 's/^/    /'
            fi

            if $CI_MODE; then
                echo ""
                echo "CI Mode: Stopping on first failure"
                echo "Full log: $test_log"
                exit 1
            fi

            return 1
        fi
    else
        local exit_code=$?
        if [[ $exit_code -eq 124 ]]; then
            log_fail "$test_name - Timeout after ${total_timeout}s"
        else
            log_fail "$test_name - Crashed (exit code: $exit_code)"
        fi

        FAILED=$((FAILED + 1))
        FAILURES+=("$test_name")

        # Show last lines of log
        if $VERBOSE && [[ -f "$test_log" ]]; then
            echo "  Last 10 lines of log:"
            tail -10 "$test_log" | sed 's/^/    /'
        fi

        if $CI_MODE; then
            echo ""
            echo "CI Mode: Stopping on first failure"
            echo "Full log: $test_log"
            exit 1
        fi

        return 1
    fi
}

# Run all tests
run_all_tests() {
    local total_tests=0

    # Count tests
    total_tests=${#SIZES[@]}
    if $TEST_FAILURES; then
        if $ALL_FAILURES; then
            total_tests=$((total_tests + ${#SIZES[@]} * ${#FAILURE_MODES[@]}))
        else
            # Just crash-leader and partition-leader
            total_tests=$((total_tests + ${#SIZES[@]} * 2))
        fi
    fi

    echo ""
    echo -e "${BOLD}====================================================${NC}"
    echo -e "${BOLD}  Kraft Linearizability Test Suite${NC}"
    echo -e "${BOLD}====================================================${NC}"
    echo ""
    echo "Profile:       $TEST_PROFILE"
    echo "Cluster sizes: ${SIZES[*]}"
    echo "Failure tests: $TEST_FAILURES"
    echo "Total tests:   $total_tests"
    echo "Results:       $RESULTS_DIR"
    echo ""
    echo -e "${BOLD}----------------------------------------------------${NC}"
    echo ""

    # Phase 1: Basic linearizability tests (no failures)
    log "Phase 1: Basic Linearizability Tests"
    echo ""

    for size in "${SIZES[@]}"; do
        run_linearizability_test $size "none"
    done

    echo ""

    # Phase 2: Failure injection tests
    if $TEST_FAILURES; then
        log "Phase 2: Failure Injection Tests"
        echo ""

        local failure_modes_to_test=("crash-leader" "partition-leader")
        if $ALL_FAILURES; then
            failure_modes_to_test=("${FAILURE_MODES[@]}")
        fi

        for size in "${SIZES[@]}"; do
            for failure in "${failure_modes_to_test[@]}"; do
                # Skip large clusters for expensive failure tests
                if [[ $size -gt 15 ]] && [[ "$failure" == "partition-minority" ]]; then
                    log_skip "lin-${size}n-${failure} (skipped for large clusters)"
                    SKIPPED=$((SKIPPED + 1))
                    continue
                fi

                run_linearizability_test $size "$failure"
            done
        done

        echo ""
    fi
}

# Generate summary report
generate_report() {
    local report="$RESULTS_DIR/SUMMARY.txt"
    local total=$((PASSED + FAILED + SKIPPED))

    {
        echo "========================================"
        echo "  Kraft Linearizability Test Report"
        echo "========================================"
        echo ""
        echo "Date:     $(date)"
        echo "Profile:  $TEST_PROFILE"
        echo "Sizes:    ${SIZES[*]}"
        echo "Failures: $TEST_FAILURES"
        echo ""
        echo "Results:"
        echo "  Passed:  $PASSED"
        echo "  Failed:  $FAILED"
        echo "  Skipped: $SKIPPED"
        echo "  Total:   $total"
        echo ""

        if [[ $FAILED -gt 0 ]]; then
            echo "Failed tests:"
            for failure in "${FAILURES[@]}"; do
                echo "  - $failure"
            done
            echo ""
        fi

        echo "Binaries:"
        echo "  kraft-loopy: $KRAFT_LOOPY"
        echo "  kraft-linearizability: $KRAFT_LIN"
        echo ""
        echo "Log directory: $RESULTS_DIR"
    } > "$report"

    cat "$report"
}

# Cleanup
cleanup() {
    if [[ $FAILED -eq 0 ]] && ! $KEEP_LOGS; then
        log "Cleaning up results directory (all tests passed)"
        rm -rf "$RESULTS_DIR"
    else
        log "Results preserved in: $RESULTS_DIR"
    fi
}

# Main
main() {
    check_prerequisites

    # Trap to ensure cleanup on exit
    trap cleanup EXIT

    run_all_tests

    echo ""
    echo -e "${BOLD}====================================================${NC}"
    echo -e "${BOLD}  Final Summary${NC}"
    echo -e "${BOLD}====================================================${NC}"
    echo ""

    generate_report

    echo ""

    if [[ $FAILED -eq 0 ]]; then
        echo -e "${GREEN}${BOLD}ALL TESTS PASSED - LINEARIZABILITY VERIFIED${NC}"
        exit 0
    else
        echo -e "${RED}${BOLD}$FAILED TEST(S) FAILED - SEE LOGS FOR DETAILS${NC}"
        exit 1
    fi
}

main "$@"
