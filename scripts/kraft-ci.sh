#!/bin/bash
#
# kraft-ci.sh - CI Pipeline Test Runner
#
# Runs all tests in sequence with proper error handling.
#
# Usage:
#   ./scripts/kraft-ci.sh [quick|standard|full]
#
# Profiles:
#   quick    - Integration + 3,5,7 node linearizability (~3 min)
#   standard - Integration + 3-15 node + failures (~10 min)
#   full     - All tests + large clusters (~20 min)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Default profile
PROFILE="${1:-standard}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

log() {
    echo -e "${BLUE}[CI]${NC} $*"
}

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $*"
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $*"
}

# Track start time
START_TIME=$(date +%s)

echo ""
echo -e "${BOLD}======================================${NC}"
echo -e "${BOLD}  Kraft CI Pipeline${NC}"
echo -e "${BOLD}======================================${NC}"
echo ""
echo "Profile: $PROFILE"
echo "Started: $(date)"
echo ""

# Ensure binaries exist
log "Checking build..."
if [[ ! -x "$PROJECT_ROOT/build2/src/loopy-demo/kraft-loopy" ]]; then
    log "Building kraft-loopy..."
    cmake -B "$PROJECT_ROOT/build2" -S "$PROJECT_ROOT" > /dev/null 2>&1
    make -C "$PROJECT_ROOT/build2" -j4 kraft-loopy > /dev/null 2>&1
fi

if [[ ! -x "$PROJECT_ROOT/build2/src/kraft-linearizability" ]]; then
    log "Building kraft-linearizability..."
    make -C "$PROJECT_ROOT/build2" -j4 kraft-linearizability > /dev/null 2>&1
fi

log_pass "Build complete"
echo ""

# Set environment
export KRAFT_BINARY="$PROJECT_ROOT/build2/src/loopy-demo/kraft-loopy"
export KRAFT_LOOPY_BINARY="$PROJECT_ROOT/build2/src/loopy-demo/kraft-loopy"
export KRAFT_LIN_BINARY="$PROJECT_ROOT/build2/src/kraft-linearizability"
export KRAFT_BASE_PORT="${KRAFT_BASE_PORT:-15000}"

FAILED=0

# Phase 1: Integration tests
echo -e "${BOLD}Phase 1: Integration Tests${NC}"
echo "--------------------------------------"
if "$SCRIPT_DIR/kraft-test-live.sh"; then
    log_pass "Integration tests"
else
    log_fail "Integration tests"
    FAILED=1
fi
echo ""

# Phase 2: Linearizability tests
echo -e "${BOLD}Phase 2: Linearizability Tests${NC}"
echo "--------------------------------------"

case $PROFILE in
    quick)
        if "$SCRIPT_DIR/kraft-test-linearizability.sh" --quick --ci; then
            log_pass "Quick linearizability"
        else
            log_fail "Quick linearizability"
            FAILED=1
        fi
        ;;
    standard)
        if "$SCRIPT_DIR/kraft-test-linearizability.sh" --standard --failures --ci; then
            log_pass "Standard linearizability with failures"
        else
            log_fail "Standard linearizability with failures"
            FAILED=1
        fi
        ;;
    full)
        if "$SCRIPT_DIR/kraft-test-linearizability.sh" --full --failures --ci; then
            log_pass "Full linearizability with failures"
        else
            log_fail "Full linearizability with failures"
            FAILED=1
        fi
        ;;
    *)
        log_fail "Unknown profile: $PROFILE"
        echo "Usage: $0 [quick|standard|full]"
        exit 1
        ;;
esac
echo ""

# Final summary
END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))
MINUTES=$((DURATION / 60))
SECONDS=$((DURATION % 60))

echo -e "${BOLD}======================================${NC}"
echo -e "${BOLD}  CI Pipeline Summary${NC}"
echo -e "${BOLD}======================================${NC}"
echo ""
echo "Duration: ${MINUTES}m ${SECONDS}s"
echo "Profile:  $PROFILE"
echo ""

if [[ $FAILED -eq 0 ]]; then
    echo -e "${GREEN}${BOLD}ALL CI TESTS PASSED${NC}"
    exit 0
else
    echo -e "${RED}${BOLD}CI PIPELINE FAILED${NC}"
    exit 1
fi
