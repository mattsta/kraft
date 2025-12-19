# Kraft Test Harnesses

Comprehensive test automation for the Kraft Raft consensus library.

## Test Scripts

### kraft-test-live.sh

Integration tests for live Kraft clusters with client API verification.

```bash
# Run all 7 integration tests
KRAFT_BINARY=build2/src/loopy-demo/kraft-loopy ./scripts/kraft-test-live.sh

# With custom base port
KRAFT_BASE_PORT=9000 ./scripts/kraft-test-live.sh

# Keep logs on success
KEEP_LOGS=1 ./scripts/kraft-test-live.sh
```

**Tests included:**
1. Single node lifecycle
2. 3-node cluster lifecycle
3. Leader failure handling
4. Cluster consistency
5. Client API commands (SET/GET/DEL/INCR)
6. Admin commands (STATUS/SNAPSHOT/INFO)
7. Client API cluster startup

### kraft-test-linearizability.sh

Linearizability verification across cluster sizes with optional failure injection.

```bash
# Quick tests (3,5,7 nodes)
./scripts/kraft-test-linearizability.sh --quick

# Standard tests (3-15 nodes, no failures)
./scripts/kraft-test-linearizability.sh --standard

# Standard with failure injection
./scripts/kraft-test-linearizability.sh --standard --failures

# Full scale (3-27 nodes)
./scripts/kraft-test-linearizability.sh --full

# Stress testing (3-69 nodes)
./scripts/kraft-test-linearizability.sh --stress

# All failure modes
./scripts/kraft-test-linearizability.sh --standard --all-failures

# CI mode (stop on first failure)
./scripts/kraft-test-linearizability.sh --standard --ci

# Verbose output
./scripts/kraft-test-linearizability.sh --quick --verbose

# Keep logs even on success
./scripts/kraft-test-linearizability.sh --quick --keep-logs
```

**Test profiles:**
- `--quick`: 3, 5, 7 nodes (fast CI check)
- `--standard`: 3, 5, 7, 11, 15 nodes
- `--full`: 3, 5, 7, 9, 11, 15, 21, 27 nodes
- `--stress`: 3, 5, 7, 9, 11, 15, 21, 27, 33, 45, 51, 69 nodes

**Failure modes:**
- `crash-leader`: Kill leader node with SIGKILL
- `crash-follower`: Kill a follower node
- `partition-leader`: Isolate leader with SIGSTOP
- `partition-minority`: Isolate minority of nodes

### test-scale-validation.sh

Comprehensive cluster size validation (3-27 nodes).

```bash
# Run scale validation
./scripts/test-scale-validation.sh
```

**Tests cluster sizes:** 3, 5, 7, 11, 15, 21, 27 nodes

### kraft-cluster.sh

Interactive cluster management for development.

```bash
# Start a 5-node cluster
./scripts/kraft-cluster.sh start 5

# Stop cluster
./scripts/kraft-cluster.sh stop

# Status
./scripts/kraft-cluster.sh status
```

### kraft-tmux.sh

Tmux-based cluster visualization for debugging.

```bash
# Start 3-node cluster in tmux
./scripts/kraft-tmux.sh 3
```

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `KRAFT_BINARY` | Path to kraft-loopy binary | `build2/src/loopy-demo/kraft-loopy` |
| `KRAFT_LOOPY_BINARY` | Same as KRAFT_BINARY | auto-detect |
| `KRAFT_LIN_BINARY` | Path to kraft-linearizability | `build2/src/kraft-linearizability` |
| `KRAFT_BASE_PORT` | Base port for Raft communication | `9000` |
| `KEEP_LOGS` | Keep logs even on success | `0` |

## CI Integration

### Quick CI Check (~3 minutes)

```bash
# Integration + quick linearizability
./scripts/kraft-test-live.sh && ./scripts/kraft-test-linearizability.sh --quick --ci
```

### Standard CI Check (~10 minutes)

```bash
# Full integration + standard linearizability with failures
./scripts/kraft-test-live.sh && \
./scripts/kraft-test-linearizability.sh --standard --failures --ci
```

### Full CI Check (~20 minutes)

```bash
# All tests including large clusters
./scripts/kraft-test-live.sh && \
./scripts/kraft-test-linearizability.sh --full --failures --ci
```

## CMake/CTest Integration

```bash
cd build2

# Run all tests
ctest

# Run only linearizability tests
ctest -L linearizability

# Run quick linearizability preset
ctest -R Quick

# Run with verbose output
ctest -V -R Linearizability
```

## Results and Logs

Test results are saved to:
- Integration: `build2/kraft-test-*/`
- Linearizability: `build2/linearizability-tests-*/`
- Scale validation: `build2/scale-test-results/`

Each test run creates a timestamped directory with:
- Per-node logs (`logs/node*.log`)
- JSON results for linearizability tests
- Summary report (`SUMMARY.txt`)

Logs are automatically cleaned up on success unless `KEEP_LOGS=1` or `--keep-logs`.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | All tests passed |
| 1 | One or more tests failed |
| 124 | Test timed out |
| 127 | Binary not found |

## Troubleshooting

### Port conflicts

Use `KRAFT_BASE_PORT` to specify a unique port range:

```bash
KRAFT_BASE_PORT=12000 ./scripts/kraft-test-live.sh
```

### Slow startup

Large clusters (>15 nodes) may need longer startup times. The scripts automatically scale timeouts.

### Debug logging

Set verbose mode for detailed output:

```bash
./scripts/kraft-test-linearizability.sh --quick --verbose --keep-logs
```

Then check the logs in the results directory.
