#!/bin/bash
#
# test-scale-validation.sh - Comprehensive cluster size validation
#
# Tests linearizability across multiple cluster sizes to ensure
# correctness and scalability from small to large deployments.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Use absolute paths to avoid directory issues
KRAFT_LOOPY="${KRAFT_LOOPY_BINARY:-$PROJECT_ROOT/build2/src/loopy-demo/kraft-loopy}"
KRAFT_LIN="${KRAFT_LIN_BINARY:-$PROJECT_ROOT/build2/src/kraft-linearizability}"

export KRAFT_LOOPY_BINARY="$KRAFT_LOOPY"
RESULTS_DIR="$PROJECT_ROOT/build2/scale-test-results"
mkdir -p "$RESULTS_DIR"

# Cluster sizes to test
SIZES=(3 5 7 11 15 21 27)

# Test configurations for each size
QUICK_OPS=500
STANDARD_OPS=1000

echo "==============================================================="
echo "  Kraft Cluster Scale Validation"
echo "==============================================================="
echo ""
echo "Testing cluster sizes: ${SIZES[*]}"
echo "Results will be saved to: $RESULTS_DIR"
echo ""

PASSED=0
FAILED=0
RESULTS_FILE="$RESULTS_DIR/summary.txt"

{
    echo "Kraft Cluster Scale Validation Results"
    echo "========================================"
    echo "Date: $(date)"
    echo "Binary: $KRAFT_LOOPY"
    echo ""
} > "$RESULTS_FILE"

for SIZE in "${SIZES[@]}"; do
    echo "------------------------------------------------"
    echo "Testing cluster size: $SIZE nodes"
    echo "------------------------------------------------"

    # Choose operation count based on cluster size
    if [ "$SIZE" -le 7 ]; then
        OPS=$STANDARD_OPS
        CLIENTS=5
    else
        OPS=$QUICK_OPS
        CLIENTS=3
    fi

    TEST_LOG="$RESULTS_DIR/size-${SIZE}.log"
    TEST_JSON="$RESULTS_DIR/size-${SIZE}.json"
    TEST_DATA="$RESULTS_DIR/data-${SIZE}"
    mkdir -p "$TEST_DATA"

    # Scale timeout with cluster size
    if [ "$SIZE" -le 7 ]; then
        TEST_TIMEOUT=120
    elif [ "$SIZE" -le 15 ]; then
        TEST_TIMEOUT=180
    else
        TEST_TIMEOUT=300
    fi

    echo "  Operations: $OPS"
    echo "  Clients: $CLIENTS"
    echo "  Timeout: ${TEST_TIMEOUT}s"
    echo "  Log: $TEST_LOG"

    # Run linearizability test with unique data directory
    if timeout $TEST_TIMEOUT $KRAFT_LIN \
        --nodes "$SIZE" \
        --clients "$CLIENTS" \
        --ops "$OPS" \
        --time 60s \
        --data-dir "$TEST_DATA" \
        --json "$TEST_JSON" \
        > "$TEST_LOG" 2>&1; then

        # Check if linearizability passed
        if grep -q "RESULT: PASS" "$TEST_LOG"; then
            echo "  ✓ PASS - Linearizable"
            PASSED=$((PASSED + 1))

            # Extract statistics
            COMPLETIONS=$(grep "Completions:" "$TEST_LOG" | awk '{print $2}')
            TIMEOUTS=$(grep "Timeouts:" "$TEST_LOG" | awk '{print $2}')
            SUCCESS_RATE=$(grep "Success rate:" "$TEST_LOG" | awk '{print $3}')

            {
                echo "[$SIZE nodes] PASS"
                echo "  Completions: $COMPLETIONS/$OPS"
                echo "  Timeouts: $TIMEOUTS"
                echo "  Success rate: $SUCCESS_RATE"
                echo ""
            } >> "$RESULTS_FILE"

        else
            echo "  ✗ FAIL - Violation detected"
            FAILED=$((FAILED + 1))

            {
                echo "[$SIZE nodes] FAIL - Linearizability violation"
                echo "  See: $TEST_LOG"
                echo ""
            } >> "$RESULTS_FILE"
        fi
    else
        echo "  ✗ FAIL - Test crashed or timed out"
        FAILED=$((FAILED + 1))

        {
            echo "[$SIZE nodes] FAIL - Crash/timeout"
            echo "  See: $TEST_LOG"
            echo ""
        } >> "$RESULTS_FILE"
    fi

    echo ""
done

echo "==============================================================="
echo "  Final Results"
echo "==============================================================="
cat "$RESULTS_FILE"
echo ""
echo "Summary: $PASSED passed, $FAILED failed out of ${#SIZES[@]} tests"
echo "All logs saved to: $RESULTS_DIR"
echo ""

if [ "$FAILED" -eq 0 ]; then
    echo "✓ ALL CLUSTER SIZES VALIDATED - LINEARIZABILITY GUARANTEED"
    exit 0
else
    echo "✗ FAILURES DETECTED - SEE LOGS FOR DETAILS"
    exit 1
fi
