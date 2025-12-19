# Production Deployment

This guide covers deploying Kraft clusters in production environments.

## Cluster Sizing

### Node Count

```
┌─────────────────────────────────────────────────────────────────┐
│                   Cluster Size Tradeoffs                         │
│                                                                  │
│  Nodes │ Quorum │ Fault Tolerance │ Write Latency │ Use Case    │
│  ──────┼────────┼─────────────────┼───────────────┼───────────  │
│    3   │   2    │ 1 node          │ Lowest        │ Development │
│    5   │   3    │ 2 nodes         │ Medium        │ Production  │
│    7   │   4    │ 3 nodes         │ Higher        │ Critical    │
│                                                                  │
│  Recommendation: Use 5 nodes for production                     │
│  • Survives 2 simultaneous failures                             │
│  • Good balance of durability and performance                   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Hardware Requirements

```
┌─────────────────────────────────────────────────────────────────┐
│                   Hardware Guidelines                            │
│                                                                  │
│  Component │ Minimum      │ Recommended   │ High Performance    │
│  ──────────┼──────────────┼───────────────┼──────────────────   │
│  CPU       │ 2 cores      │ 4 cores       │ 8+ cores            │
│  RAM       │ 2 GB         │ 8 GB          │ 32+ GB              │
│  Disk      │ 20 GB SSD    │ 100 GB NVMe   │ 500+ GB NVMe        │
│  Network   │ 1 Gbps       │ 10 Gbps       │ 25+ Gbps            │
│                                                                  │
│  Storage Notes:                                                  │
│  • SSD/NVMe strongly recommended (fsync latency critical)       │
│  • Separate disk for WAL if possible                            │
│  • Size = (log retention × write rate) + snapshots              │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Network Configuration

### Port Allocation

```c
// Default ports
#define KRAFT_PORT_RAFT       7001    // Raft protocol (node-to-node)
#define KRAFT_PORT_CLIENT     7002    // Client API
#define KRAFT_PORT_ADMIN      7003    // Admin/metrics

// Firewall rules needed:
// - Allow 7001 between all cluster nodes
// - Allow 7002 from clients
// - Allow 7003 from monitoring systems
```

### Network Layout

```
┌─────────────────────────────────────────────────────────────────┐
│                   Recommended Network Layout                     │
│                                                                  │
│                    ┌─────────────────┐                          │
│                    │  Load Balancer  │                          │
│                    └────────┬────────┘                          │
│                             │ :7002 (clients)                   │
│           ┌─────────────────┼─────────────────┐                 │
│           │                 │                 │                 │
│           ▼                 ▼                 ▼                 │
│    ┌─────────────┐   ┌─────────────┐   ┌─────────────┐         │
│    │   Node 1    │   │   Node 2    │   │   Node 3    │         │
│    │ :7001 :7002 │   │ :7001 :7002 │   │ :7001 :7002 │         │
│    └──────┬──────┘   └──────┬──────┘   └──────┬──────┘         │
│           │                 │                 │                 │
│           └─────────────────┼─────────────────┘                 │
│                             │ :7001 (raft)                      │
│                    Private Network                              │
│                                                                  │
│  Key Points:                                                    │
│  • Raft traffic on private network                              │
│  • Client traffic through load balancer                         │
│  • LB should route to leader for writes                         │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### DNS Configuration

```bash
# Example DNS entries
kraft-node1.internal.example.com    A    10.0.1.1
kraft-node2.internal.example.com    A    10.0.1.2
kraft-node3.internal.example.com    A    10.0.1.3
kraft-node4.internal.example.com    A    10.0.1.4
kraft-node5.internal.example.com    A    10.0.1.5

# Client-facing (points to load balancer)
kraft.example.com                   A    203.0.113.1
```

---

## Configuration

### Server Configuration

```c
// Production server configuration
kraftServerConfig config = {
    // Identity
    .nodeId = 1,
    .clusterId = 0xCAFEBABE,

    // Network
    .bindAddress = "0.0.0.0",
    .raftPort = 7001,
    .clientPort = 7002,
    .adminPort = 7003,

    // Timing (milliseconds)
    .electionTimeoutMin = 300,
    .electionTimeoutMax = 500,
    .heartbeatInterval = 100,

    // Storage
    .dataDir = "/var/lib/kraft/data",
    .walDir = "/var/lib/kraft/wal",
    .snapshotDir = "/var/lib/kraft/snapshots",

    // Durability
    .fsyncMode = KRAFT_FSYNC_COMMIT,
    .commitMode = KRAFT_COMMIT_SYNC,

    // Features
    .enablePreVote = true,
    .enableReadIndex = true,
    .enableLeadershipTransfer = true,

    // Limits
    .maxLogSize = 10 * 1024 * 1024 * 1024LL,  // 10GB
    .snapshotThreshold = 100000,               // entries
    .maxConnections = 10000,
};
```

### Environment Variables

```bash
# /etc/kraft/kraft.env
KRAFT_NODE_ID=1
KRAFT_CLUSTER_ID=0xCAFEBABE
KRAFT_DATA_DIR=/var/lib/kraft/data
KRAFT_WAL_DIR=/var/lib/kraft/wal
KRAFT_BIND_ADDRESS=0.0.0.0
KRAFT_RAFT_PORT=7001
KRAFT_CLIENT_PORT=7002
KRAFT_ADMIN_PORT=7003
KRAFT_ELECTION_TIMEOUT_MIN=300
KRAFT_ELECTION_TIMEOUT_MAX=500
KRAFT_HEARTBEAT_INTERVAL=100
```

### Systemd Service

```ini
# /etc/systemd/system/kraft.service
[Unit]
Description=Kraft Raft Consensus Server
After=network.target

[Service]
Type=simple
User=kraft
Group=kraft
EnvironmentFile=/etc/kraft/kraft.env
ExecStart=/usr/bin/kraft-server
ExecReload=/bin/kill -HUP $MAINPID
Restart=on-failure
RestartSec=5
LimitNOFILE=65536
LimitNPROC=4096

# Security hardening
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths=/var/lib/kraft /var/log/kraft

[Install]
WantedBy=multi-user.target
```

---

## Storage Layout

### Directory Structure

```
/var/lib/kraft/
├── data/                    # Main data directory
│   ├── log/                 # Raft log entries
│   │   ├── segment-0001     # Log segments
│   │   ├── segment-0002
│   │   └── ...
│   └── state/               # Persistent state
│       ├── term             # Current term
│       └── votedFor         # Vote record
├── wal/                     # Write-ahead log (optional, separate disk)
│   └── wal-current
└── snapshots/               # Snapshots
    ├── snapshot-1000-5.dat  # Snapshot at index 1000, term 5
    └── snapshot-2000-7.dat
```

### Disk Layout

```
┌─────────────────────────────────────────────────────────────────┐
│              Recommended Disk Configuration                      │
│                                                                  │
│  Single Disk (Simple):                                          │
│  /dev/sda1 → /var/lib/kraft (all data)                         │
│                                                                  │
│  Dual Disk (Recommended):                                       │
│  /dev/nvme0n1 → /var/lib/kraft/wal    (WAL, fastest disk)      │
│  /dev/nvme1n1 → /var/lib/kraft/data   (data + snapshots)       │
│                                                                  │
│  Triple Disk (Maximum Performance):                             │
│  /dev/nvme0n1 → /var/lib/kraft/wal       (WAL)                 │
│  /dev/nvme1n1 → /var/lib/kraft/data      (log data)            │
│  /dev/ssd0    → /var/lib/kraft/snapshots (snapshots)           │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Security

### TLS Configuration

```c
kraftTLSConfig tls = {
    .enabled = true,
    .certFile = "/etc/kraft/certs/server.crt",
    .keyFile = "/etc/kraft/certs/server.key",
    .caFile = "/etc/kraft/certs/ca.crt",
    .clientAuth = true,  // Mutual TLS
    .minVersion = TLS_1_2,
};

kraftServerConfigureTLS(server, &tls);
```

### Certificate Generation

```bash
# Generate CA
openssl genrsa -out ca.key 4096
openssl req -new -x509 -days 3650 -key ca.key -out ca.crt \
    -subj "/CN=Kraft CA"

# Generate server certificate for each node
for i in 1 2 3 4 5; do
    openssl genrsa -out node${i}.key 2048
    openssl req -new -key node${i}.key -out node${i}.csr \
        -subj "/CN=kraft-node${i}.internal.example.com"
    openssl x509 -req -days 365 -in node${i}.csr \
        -CA ca.crt -CAkey ca.key -CAcreateserial \
        -out node${i}.crt
done
```

### Authentication

```c
// Client authentication
kraftAuthConfig auth = {
    .type = KRAFT_AUTH_TOKEN,
    .tokenValidator = validateClientToken,
    .userData = authContext,
};

kraftServerConfigureAuth(server, &auth);

bool validateClientToken(const char *token, size_t len, void *ctx) {
    // Validate against your auth system
    return checkTokenWithAuthService(token, len);
}
```

---

## High Availability

### Cross-Datacenter Deployment

```
┌─────────────────────────────────────────────────────────────────┐
│                  Cross-DC Configuration                          │
│                                                                  │
│  Datacenter A (Primary)     Datacenter B         Datacenter C   │
│  ┌─────────────────────┐   ┌──────────────┐    ┌──────────────┐│
│  │ Node 1 (VOTER)      │   │ Node 4       │    │ Node 5       ││
│  │ Node 2 (VOTER)      │   │ (VOTER)      │    │ (VOTER)      ││
│  │ Node 3 (VOTER)      │   │              │    │              ││
│  └─────────────────────┘   └──────────────┘    └──────────────┘│
│                                                                  │
│  Quorum requires 3/5 nodes                                      │
│  • DC-A failure: B+C form quorum (2 nodes)                      │
│  • Any single DC failure: remaining form quorum                 │
│                                                                  │
│  Alternative: 3 DCs with 3 nodes each (9 total)                 │
│  • Any single DC failure: 6 nodes remain, quorum = 5            │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Witness Deployment

```c
// Add witnesses for read scalability and DR
kraftWitnessConfig witnesses[] = {
    {
        .nodeId = 101,
        .address = "witness1.dc-b.example.com",
        .port = 7001,
        .syncMode = KRAFT_WITNESS_ASYNC,
    },
    {
        .nodeId = 102,
        .address = "witness2.dc-c.example.com",
        .port = 7001,
        .syncMode = KRAFT_WITNESS_ASYNC,
    },
};

for (int i = 0; i < 2; i++) {
    kraftStateAddWitness(state, &witnesses[i]);
}
```

---

## Operational Procedures

### Initial Bootstrap

```bash
# 1. Start first node (becomes initial leader)
./kraft-server --node-id=1 --bootstrap

# 2. Start remaining nodes (join cluster)
./kraft-server --node-id=2 --join=node1.example.com:7001
./kraft-server --node-id=3 --join=node1.example.com:7001
./kraft-server --node-id=4 --join=node1.example.com:7001
./kraft-server --node-id=5 --join=node1.example.com:7001
```

### Rolling Restart

```bash
#!/bin/bash
# Rolling restart script

NODES="node1 node2 node3 node4 node5"

for node in $NODES; do
    echo "Restarting $node..."

    # Transfer leadership if this node is leader
    ssh $node "kraft-ctl transfer-leadership"

    # Wait for transfer
    sleep 5

    # Restart
    ssh $node "systemctl restart kraft"

    # Wait for node to rejoin and catch up
    echo "Waiting for $node to catch up..."
    while ! ssh $node "kraft-ctl health" | grep -q "healthy"; do
        sleep 1
    done

    echo "$node is healthy"
    sleep 5  # Brief pause before next node
done

echo "Rolling restart complete"
```

### Adding a Node

```bash
# 1. Start new node pointing to existing cluster
./kraft-server --node-id=6 --join=node1.example.com:7001

# 2. Wait for node to sync
kraft-ctl wait-sync --node=6

# 3. Add as voting member (from any cluster node)
kraft-ctl add-voter --node-id=6 \
    --address=node6.example.com --port=7001
```

### Removing a Node

```bash
# 1. If removing leader, transfer leadership first
kraft-ctl transfer-leadership --target=2

# 2. Remove from cluster
kraft-ctl remove-voter --node-id=5

# 3. Shutdown the node
ssh node5 "systemctl stop kraft"
```

---

## Backup and Recovery

### Snapshot Backup

```bash
#!/bin/bash
# Backup latest snapshot

SNAPSHOT_DIR="/var/lib/kraft/snapshots"
BACKUP_DIR="/backup/kraft/$(date +%Y%m%d)"

mkdir -p $BACKUP_DIR

# Find latest snapshot
LATEST=$(ls -t $SNAPSHOT_DIR/snapshot-*.dat | head -1)

if [ -n "$LATEST" ]; then
    cp "$LATEST" "$BACKUP_DIR/"
    echo "Backed up: $LATEST"
else
    echo "No snapshot found"
    exit 1
fi

# Also backup current state
kraft-ctl snapshot  # Force snapshot if needed
```

### Full Cluster Restore

```bash
#!/bin/bash
# Restore cluster from backup

BACKUP_DIR="/backup/kraft/20240115"

# 1. Stop all nodes
for node in node1 node2 node3 node4 node5; do
    ssh $node "systemctl stop kraft"
done

# 2. Restore snapshot to all nodes
for node in node1 node2 node3 node4 node5; do
    ssh $node "rm -rf /var/lib/kraft/data/*"
    scp $BACKUP_DIR/snapshot-*.dat $node:/var/lib/kraft/snapshots/
done

# 3. Bootstrap from snapshot
ssh node1 "kraft-server --bootstrap-from-snapshot"

# 4. Start remaining nodes
for node in node2 node3 node4 node5; do
    ssh $node "kraft-server --join=node1.example.com:7001"
done
```

---

## Capacity Planning

### Write Throughput

```
┌─────────────────────────────────────────────────────────────────┐
│                   Write Capacity Estimation                      │
│                                                                  │
│  Factors:                                                        │
│  • Entry size (bytes)                                           │
│  • Fsync latency (ms)                                           │
│  • Network RTT between nodes (ms)                               │
│  • Batch size                                                   │
│                                                                  │
│  Formula (simplified):                                          │
│  Writes/sec ≈ batch_size / (fsync_ms + rtt_ms)                 │
│                                                                  │
│  Example (5-node cluster):                                      │
│  • fsync: 5ms (NVMe)                                           │
│  • RTT: 1ms (same DC)                                          │
│  • Batch: 100 entries                                          │
│  → ~16,000 writes/sec                                          │
│                                                                  │
│  With LEADER_ONLY commit:                                       │
│  → ~100,000+ writes/sec (but reduced durability)               │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Storage Sizing

```
Storage needed = (entries/day × entry_size × retention_days)
                 + (snapshot_size × snapshot_count)

Example:
• 1M entries/day × 500 bytes × 7 days = 3.5 GB logs
• 500 MB snapshots × 3 retained = 1.5 GB snapshots
• Total: ~5 GB + 50% headroom = 7.5 GB minimum
```

---

## Next Steps

- [Monitoring](monitoring.md) - Metrics and observability
- [Troubleshooting](troubleshooting.md) - Common issues
- [Configuration Reference](../reference/configuration.md) - All options
