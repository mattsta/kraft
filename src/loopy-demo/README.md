# Kraft Loopy Demo

A production-ready Raft consensus demonstration using the **kraft** library and **loopy** event loop.

## Overview

Kraft Loopy Demo showcases how to build distributed consensus applications on top of the kraft Raft library. It provides:

- **Interactive CLI** with a built-in key-value store
- **Client API** for external TCP connections
- **Multi-node clustering** with automatic leader election
- **Live status monitoring** of cluster state
- **Membership changes** (add/remove nodes at runtime)
- **Snapshot support** for log compaction
- **Leadership transfer** for graceful failover

## Quick Start

### Single Node (Development)

```bash
./kraft-loopy --listen 127.0.0.1:5001 --node 1 --data /tmp/node1
```

### Three-Node Cluster

Start each node in a separate terminal:

```bash
# Terminal 1
./kraft-loopy --listen 127.0.0.1:5001 --node 1 \
  --cluster 127.0.0.1:5001,127.0.0.1:5002,127.0.0.1:5003 \
  --data /tmp/node1

# Terminal 2
./kraft-loopy --listen 127.0.0.1:5002 --node 2 \
  --cluster 127.0.0.1:5001,127.0.0.1:5002,127.0.0.1:5003 \
  --data /tmp/node2

# Terminal 3
./kraft-loopy --listen 127.0.0.1:5003 --node 3 \
  --cluster 127.0.0.1:5001,127.0.0.1:5002,127.0.0.1:5003 \
  --data /tmp/node3
```

### Using the Cluster Script

```bash
# Start a 5-node cluster
KRAFT_BINARY=./build2/src/loopy-demo/kraft-loopy scripts/kraft-cluster.sh start 5

# Check status
scripts/kraft-cluster.sh status

# Stop the cluster
scripts/kraft-cluster.sh stop
```

## Features

### Key-Value Store Commands

| Command         | Description             | Example          |
| --------------- | ----------------------- | ---------------- |
| `SET key value` | Store a value           | `SET user alice` |
| `GET key`       | Retrieve a value        | `GET user`       |
| `DEL key`       | Delete a key            | `DEL user`       |
| `INCR key`      | Increment numeric value | `INCR counter`   |
| `KEYS`          | List all keys           | `KEYS`           |

### Cluster Management Commands

| Command            | Description             | Example                |
| ------------------ | ----------------------- | ---------------------- |
| `STATUS`           | Full cluster status     | `STATUS`               |
| `STAT`             | Brief statistics        | `STAT`                 |
| `SNAPSHOT`         | Trigger manual snapshot | `SNAPSHOT`             |
| `TRANSFER node`    | Transfer leadership     | `TRANSFER 2`           |
| `ADD id addr:port` | Add cluster member      | `ADD 4 127.0.0.1:5004` |
| `REMOVE id`        | Remove cluster member   | `REMOVE 4`             |
| `QUIT`             | Graceful shutdown       | `QUIT`                 |

### Raft Extensions (Enabled by Default)

- **PreVote (Section 9.6)**: Prevents disruption from partitioned nodes rejoining
- **ReadIndex**: Linearizable reads without log replication overhead

## Client API

The Client API provides a separate TCP port for external applications to connect and perform key-value operations. This allows programmatic access to the cluster without using the interactive CLI.

### Enabling Client API

```bash
# Start with client API on port 5101
./kraft-loopy --listen 127.0.0.1:5001 --node 1 --client 5101

# Cluster with client API ports
./kraft-loopy --listen 127.0.0.1:5001 --node 1 --client 5101 \
  --cluster 127.0.0.1:5001,127.0.0.1:5002,127.0.0.1:5003
```

### Protocol

The Client API uses a text protocol:

**Key-Value Commands** (sent as `COMMAND args\r\n`):

```
SET key value       # Store a value (write - requires leader)
GET key             # Retrieve a value (read - any node)
DEL key             # Delete a key (write - requires leader)
INCR key            # Increment numeric value (write - requires leader)
STAT                # Get KV store statistics
```

**Cluster Management Commands**:

```
STATUS              # Full cluster status (role, term, leader, etc.)
INFO                # Alias for STATUS
SNAPSHOT            # Trigger manual snapshot creation
TRANSFER <node-id>  # Transfer leadership to another node (leader only)
ADD <id> <addr:port># Add node to cluster (leader only)
REMOVE <id>         # Remove node from cluster (leader only)
```

**Responses**:

| Response           | Meaning               | Example                           |
| ------------------ | --------------------- | --------------------------------- |
| `+OK\r\n`          | Success               | After SET, DEL, TRANSFER          |
| `$N\r\ndata\r\n`   | Bulk string (N bytes) | GET, STATUS, SNAPSHOT response    |
| `:N\r\n`           | Integer               | INCR result                       |
| `$-1\r\n`          | Null                  | GET for missing key               |
| `-ERR message\r\n` | Error                 | Invalid command, operation failed |
| `-REDIRECT N\r\n`  | Not leader            | Write command on follower         |

### Example Usage with netcat

```bash
# Connect to client API
nc 127.0.0.1 5101

# Set a value
SET mykey myvalue
+OK

# Get a value
GET mykey
$7
myvalue

# Increment counter
INCR counter
:1

# Delete key
DEL mykey
+OK

# Check cluster status
STATUS
$128
node_id:1
cluster_id:1
role:leader
term:3
leader_id:1
commit_index:42
last_applied:42
is_leader:1
kv_keys:2
kv_memory:128

# Trigger snapshot
SNAPSHOT
$48
size:256
last_index:42
last_term:3

# Transfer leadership to node 2
TRANSFER 2
+OK
```

### Programmatic Access

```bash
# Send commands via pipeline
printf "SET foo bar\r\n" | nc -w2 127.0.0.1 5101
printf "GET foo\r\n" | nc -w2 127.0.0.1 5101
printf "INCR visits\r\n" | nc -w2 127.0.0.1 5101

# Check cluster health
printf "STATUS\r\n" | nc -w2 127.0.0.1 5101

# Create snapshot for backup
printf "SNAPSHOT\r\n" | nc -w2 127.0.0.1 5101

# Gracefully move leadership before maintenance
printf "TRANSFER 2\r\n" | nc -w2 127.0.0.1 5101

# Add a new node to the cluster
printf "ADD 4 127.0.0.1:5004\r\n" | nc -w2 127.0.0.1 5101

# Remove a node from the cluster
printf "REMOVE 4\r\n" | nc -w2 127.0.0.1 5101
```

### Port Configuration

When using the cluster scripts with client API:

- Raft ports: `KRAFT_BASE_PORT` (default 5001, 5002, 5003...)
- Client ports: `KRAFT_BASE_PORT + 100` (default 5101, 5102, 5103...)

## Architecture

```
+------------------+        +---------------------------+
|   kraft (core)   |        |   loopy-demo (network)    |
|------------------|        |---------------------------|
| - Raft consensus | <----- | - TCP/loopy networking    |
| - Log storage    |  uses  | - Timer management        |
| - RPC processing |        | - Connection handling     |
+------------------+        +---------------------------+
```

### Module Structure

```
src/loopy-demo/
  main.c            - CLI entry point, KV store demo
  loopyServer.c     - High-level Raft server API
  loopyPlatform.c   - Server lifecycle, event loop owner
  loopyTransport.c  - TCP server + outbound connections
  loopyConnection.c - Per-connection I/O management
  loopySession.c    - Node protocol state, handshake
  loopyTimers.c     - Election/heartbeat timer management
  loopyCluster.c    - Cluster membership management
  loopyKV.c         - Key-value state machine
  loopyClientAPI.c  - Client API TCP listener
```

## Use Cases

### 1. Distributed Configuration Store

Store configuration that must be consistent across all nodes:

```bash
# On leader
SET database.host "postgres.internal"
SET database.port "5432"
SET feature.newui "enabled"

# On any node - consistent read
GET database.host
```

### 2. Leader Election Service

Use kraft-loopy as a leader election primitive for other services:

```bash
# Check who is leader
STATUS
# Output shows current leader ID
```

### 3. Distributed Counters

Atomic counters that survive node failures:

```bash
INCR page_views
INCR api_calls
GET page_views
```

### 4. Service Discovery

Register services and discover them consistently:

```bash
SET service.auth.addr "10.0.1.5:8080"
SET service.auth.health "healthy"
GET service.auth.addr
```

### 5. Distributed Locks (Pattern)

Implement distributed locks using the KV store:

```bash
# Acquire lock (only succeeds if key doesn't exist)
SET lock.myresource "node1"

# Release lock
DEL lock.myresource
```

## Performance Characteristics

### Latency

| Operation       | Single Node | 3-Node Cluster | 5-Node Cluster |
| --------------- | ----------- | -------------- | -------------- |
| Write (SET/DEL) | <1ms        | 2-5ms          | 3-8ms          |
| Read (GET)      | <0.1ms      | <0.1ms         | <0.1ms         |
| Leader Election | N/A         | 150-300ms      | 150-300ms      |

### Throughput

- **Writes**: 1,000-10,000 ops/sec depending on cluster size
- **Reads**: 100,000+ ops/sec (local, no consensus required)
- **Log Compaction**: Automatic via snapshots

### Timing Configuration

| Parameter          | Default        | Description                       |
| ------------------ | -------------- | --------------------------------- |
| Election Timeout   | 150-300ms      | Randomized to prevent split votes |
| Heartbeat Interval | 50ms           | Leader keepalive frequency        |
| Snapshot Threshold | 10,000 entries | Auto-compact after N log entries  |

## Fault Tolerance

### Node Failures

- **Minority failure**: Cluster continues operating
- **Majority failure**: Cluster stops accepting writes (safety)
- **Leader failure**: New election within 150-300ms

### Network Partitions

- **PreVote prevents disruption**: Isolated nodes cannot disrupt cluster on rejoin
- **Split-brain prevention**: Only partition with majority can elect leader

### Recovery

```bash
# After node crash, simply restart
./kraft-loopy --listen 127.0.0.1:5001 --node 1 --data /tmp/node1

# State is automatically recovered from:
# 1. Snapshot (if exists)
# 2. Log replay
# 3. Catch-up from leader
```

## Live Testing

### Run All Integration Tests

```bash
./scripts/kraft-test-live.sh all
```

### Individual Tests

```bash
# Single node lifecycle
./scripts/kraft-test-live.sh single

# 3-node cluster startup
./scripts/kraft-test-live.sh cluster3

# Leader failover
./scripts/kraft-test-live.sh failover

# Consistency verification
./scripts/kraft-test-live.sh consistency

# Client API basic commands (SET, GET, DEL, INCR)
./scripts/kraft-test-live.sh client_api

# Client API admin commands (STATUS, SNAPSHOT, INFO)
./scripts/kraft-test-live.sh client_admin

# Client API in 3-node cluster
./scripts/kraft-test-live.sh client_cluster
```

### Chaos Testing

```bash
# Run fuzzer with random scenarios
./kraft-fuzz

# CRDT-style convergence tests
./kraft-fuzz-crdt
```

## Configuration Reference

### Command Line Options

| Option                      | Required | Description                    |
| --------------------------- | -------- | ------------------------------ |
| `--listen addr:port`        | Yes      | Address to bind (Raft port)    |
| `--node id`                 | Yes      | Unique node ID (1-based)       |
| `--data path`               | No       | Data directory for persistence |
| `--cluster addr1,addr2,...` | No       | Cluster member addresses       |
| `--client port`             | No       | Enable Client API on port      |

### Environment Variables

| Variable          | Description                                   |
| ----------------- | --------------------------------------------- |
| `KRAFT_BASE_PORT` | Base port for cluster scripts (default: 5001) |
| `KRAFT_BINARY`    | Path to kraft-loopy binary                    |

## Building

```bash
# From kraft root directory
mkdir -p build2 && cd build2
cmake ..
make kraft-loopy

# Binary at: build2/src/loopy-demo/kraft-loopy
```

### Dependencies

- **kraft**: Core Raft consensus library (included)
- **loopy**: Event loop library (in deps/)
- **datakit**: Data structure utilities (in deps/)

## Example Session

```
$ ./kraft-loopy --listen 127.0.0.1:5001 --node 1

Kraft Loopy Demo - Node 1
================

[NODE] Kraft node 1 started
  Cluster:   1
  Listen:    127.0.0.1:5001
  PreVote:   enabled
  ReadIndex: enabled

Commands: SET key value | GET key | DEL key | INCR key | STAT | STATUS | QUIT

> SET greeting "Hello, Raft!"
OK

> GET greeting
Hello, Raft!

> INCR counter
1

> INCR counter
2

> STATUS
=== Cluster Status ===
  Node ID:       1
  Role:          Leader
  Term:          1
  Leader ID:     1
  Commit Index:  3
  Last Applied:  3

=== Key-Value Store ===
  Total Keys:    2
  Total Ops:     4

=== Snapshots ===
  Created:       0
  Restored:      0

> QUIT
Shutting down...
[NODE] Node stopped
```

## Comparison with etcd

| Feature      | kraft-loopy       | etcd           |
| ------------ | ----------------- | -------------- |
| Protocol     | Raft              | Raft           |
| API          | CLI/TCP           | gRPC + HTTP    |
| Storage      | SQLite/Memory     | bbolt          |
| Language     | C                 | Go             |
| Binary Size  | ~500KB            | ~20MB          |
| Memory Usage | ~5MB              | ~50MB+         |
| Use Case     | Embedded/Learning | Production K8s |

kraft-loopy is designed for embedded use cases, learning Raft, and resource-constrained environments. For production Kubernetes deployments, etcd remains the standard choice.

## Further Reading

- [Raft Paper](https://raft.github.io/raft.pdf) - Original consensus algorithm
- [Raft Visualization](https://raft.github.io/) - Interactive demonstration
- [kraft Library](../kraft.h) - Core Raft implementation
- [loopy Library](../../deps/loopy/) - Event loop foundation
