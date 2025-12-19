# Troubleshooting

This guide covers common issues and their solutions when operating Kraft clusters.

## Diagnostic Commands

### Check Cluster Status

```bash
# Overall cluster health
kraft-ctl status

# Output:
# Cluster: kraft-prod
# Status: HEALTHY
# Leader: node2 (term 42)
# Nodes: 5/5 healthy
# Commit Index: 1,234,567

# Detailed node status
kraft-ctl nodes

# Output:
# ID   Address              Role      Match Index  Lag     Health
# 1    node1.example.com    Follower  1234560      7       OK
# 2    node2.example.com    Leader    1234567      -       OK
# 3    node3.example.com    Follower  1234565      2       OK
# 4    node4.example.com    Follower  1234567      0       OK
# 5    node5.example.com    Follower  1234563      4       OK
```

### Check Node Logs

```bash
# Recent logs
journalctl -u kraft -n 100

# Follow logs
journalctl -u kraft -f

# Filter by level
journalctl -u kraft | grep -E "ERROR|WARN"

# Filter by component
journalctl -u kraft | grep "election"
```

---

## Common Issues

### No Leader Elected

**Symptoms:**

- All nodes show as candidates or followers
- No writes succeeding
- `kraft_leader_id` metric is 0

**Causes & Solutions:**

```
┌─────────────────────────────────────────────────────────────────┐
│ Cause: Network partition                                         │
├─────────────────────────────────────────────────────────────────┤
│ Diagnosis:                                                       │
│   - Check connectivity between nodes                            │
│   - ping, telnet to port 7001                                   │
│   - Check firewall rules                                        │
│                                                                  │
│ Solution:                                                        │
│   - Fix network connectivity                                    │
│   - Ensure port 7001 open between all nodes                    │
│   - Check security groups / firewall rules                      │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ Cause: Clock skew                                                │
├─────────────────────────────────────────────────────────────────┤
│ Diagnosis:                                                       │
│   - Check NTP status on all nodes                               │
│   - Compare times: date +%s on each node                        │
│                                                                  │
│ Solution:                                                        │
│   - Sync clocks with NTP                                        │
│   - Increase election timeout if skew unavoidable              │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ Cause: Majority of nodes down                                    │
├─────────────────────────────────────────────────────────────────┤
│ Diagnosis:                                                       │
│   - Check which nodes are running                               │
│   - Verify quorum availability                                  │
│                                                                  │
│ Solution:                                                        │
│   - Bring more nodes online                                     │
│   - For 5-node cluster, need at least 3 running                │
└─────────────────────────────────────────────────────────────────┘
```

### Split Brain

**Symptoms:**

- Two nodes claim to be leader
- Clients seeing inconsistent data
- Term numbers jumping rapidly

**Diagnosis:**

```bash
# Check term on each node
for node in node1 node2 node3 node4 node5; do
    echo "$node: $(ssh $node kraft-ctl term)"
done

# Check if nodes can communicate
kraft-ctl connectivity-test
```

**Solutions:**

```c
// 1. Enable pre-vote (prevents term inflation)
kraftStateEnablePreVote(state, true);

// 2. Check network for asymmetric partitions
// A → B works, B → A fails

// 3. Increase election timeout if network is flaky
config.electionTimeoutMin = 500;  // Was 150
config.electionTimeoutMax = 1000; // Was 300
```

### Follower Falling Behind

**Symptoms:**

- One or more followers with high lag
- Slow commit times
- Leader sending snapshots frequently

**Diagnosis:**

```bash
# Check replication lag
kraft-ctl replication-status

# Check follower resources
ssh node3 "top -bn1 | head -20; iostat -x 1 3"

# Check network between leader and follower
iperf3 -c node3 -t 10
```

**Solutions:**

```
┌─────────────────────────────────────────────────────────────────┐
│ Cause: Slow disk on follower                                     │
├─────────────────────────────────────────────────────────────────┤
│ Solution:                                                        │
│   - Upgrade to SSD/NVMe                                         │
│   - Move WAL to faster disk                                     │
│   - Reduce fsync frequency (trade-off with durability)          │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ Cause: Network congestion                                        │
├─────────────────────────────────────────────────────────────────┤
│ Solution:                                                        │
│   - Check for bandwidth saturation                              │
│   - Enable compression for replication                          │
│   - Increase network bandwidth                                  │
│   - Use dedicated raft network interface                        │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ Cause: CPU overload on follower                                  │
├─────────────────────────────────────────────────────────────────┤
│ Solution:                                                        │
│   - Check what's consuming CPU                                  │
│   - Move other workloads off the node                          │
│   - Upgrade CPU                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### High Commit Latency

**Symptoms:**

- P99 commit latency > 100ms
- Clients timing out
- Throughput lower than expected

**Diagnosis:**

```bash
# Check latency breakdown
kraft-ctl latency-breakdown

# Output:
# Phase           P50    P95    P99
# Write to log    2ms    5ms    10ms
# Wait for ack    8ms    25ms   50ms
# Total commit    10ms   30ms   60ms

# Check fsync latency
kraft-ctl disk-stats
```

**Solutions:**

```c
// 1. Enable batching
kraftCommitConfig commit = {
    .batchSize = 100,      // Batch up to 100 entries
    .batchTimeout = 5,     // Max 5ms delay
};

// 2. Use faster fsync mode (trade-off with durability)
kraftStorageConfig storage = {
    .fsyncMode = KRAFT_FSYNC_BATCH,  // Was KRAFT_FSYNC_ALWAYS
    .batchFsyncInterval = 10,        // Fsync every 10ms
};

// 3. Enable pipelining
kraftReplicationConfig repl = {
    .pipeliningEnabled = true,
    .pipelineWindow = 10,
};
```

### Snapshot Failures

**Symptoms:**

- Snapshot creation timing out
- Disk filling up with log entries
- New nodes unable to join (can't install snapshot)

**Diagnosis:**

```bash
# Check snapshot status
kraft-ctl snapshot-status

# Check disk space
df -h /var/lib/kraft

# Check recent snapshot logs
journalctl -u kraft | grep -i snapshot
```

**Solutions:**

```
┌─────────────────────────────────────────────────────────────────┐
│ Cause: Not enough disk space                                     │
├─────────────────────────────────────────────────────────────────┤
│ Solution:                                                        │
│   - Add more disk space                                         │
│   - Reduce snapshot retention                                   │
│   - Enable snapshot compression                                 │
│   - Manually clean old snapshots                                │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ Cause: Snapshot too large                                        │
├─────────────────────────────────────────────────────────────────┤
│ Solution:                                                        │
│   - Enable incremental snapshots                                │
│   - Optimize state machine for smaller snapshots               │
│   - Increase snapshot timeout                                   │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ Cause: I/O bottleneck during snapshot                            │
├─────────────────────────────────────────────────────────────────┤
│ Solution:                                                        │
│   - Use separate disk for snapshots                             │
│   - Schedule snapshots during low-traffic periods              │
│   - Enable async snapshot writes                                │
└─────────────────────────────────────────────────────────────────┘
```

### Memory Issues

**Symptoms:**

- OOM killer terminating kraft process
- Swap usage increasing
- Slow performance

**Diagnosis:**

```bash
# Check memory usage
kraft-ctl memory-stats

# Output:
# Component        Used      Max
# Log cache        512MB     1GB
# Pending entries  128MB     256MB
# Connections      64MB      128MB
# State machine    2GB       -

# Check system memory
free -h
```

**Solutions:**

```c
// 1. Limit log cache size
kraftStorageConfig storage = {
    .logCacheSize = 256 * 1024 * 1024,  // 256MB cache
};

// 2. Limit pending entries
kraftCommitConfig commit = {
    .maxPendingEntries = 10000,
    .maxPendingBytes = 128 * 1024 * 1024,  // 128MB
};

// 3. Take more frequent snapshots (reduce log size)
kraftSnapshotConfig snapshot = {
    .triggerThreshold = 50000,  // Was 100000
};
```

---

## Recovery Procedures

### Recovering from Quorum Loss

When majority of nodes are permanently lost:

```bash
# 1. Identify surviving node(s)
kraft-ctl status

# 2. Force new cluster from surviving node
# WARNING: This resets cluster membership
kraft-ctl force-new-cluster --node=node1

# 3. Add new nodes to rebuild cluster
kraft-ctl add-voter --node-id=6 --address=node6.example.com
kraft-ctl add-voter --node-id=7 --address=node7.example.com

# 4. Verify cluster health
kraft-ctl status
```

### Recovering from Data Corruption

```bash
# 1. Stop affected node
systemctl stop kraft

# 2. Check for corruption
kraft-ctl verify-data /var/lib/kraft/data

# 3. If corrupted, restore from snapshot
rm -rf /var/lib/kraft/data/*
cp /backup/kraft/latest/snapshot-*.dat /var/lib/kraft/snapshots/

# 4. Restart and rejoin
systemctl start kraft
kraft-ctl rejoin --cluster=node1.example.com:7001
```

### Recovering Stuck Node

```bash
# 1. Check what's blocking
kraft-ctl debug-state

# 2. Force step-down if leader is stuck
kraft-ctl step-down --force

# 3. Clear stuck state
kraft-ctl clear-pending-requests
kraft-ctl clear-pending-config-change

# 4. Restart node
systemctl restart kraft
```

---

## Performance Tuning

### Disk I/O

```bash
# Check current I/O settings
cat /sys/block/nvme0n1/queue/scheduler
cat /sys/block/nvme0n1/queue/nr_requests

# Optimize for raft workload (write-heavy, sync-heavy)
echo "none" > /sys/block/nvme0n1/queue/scheduler  # For NVMe
echo 256 > /sys/block/nvme0n1/queue/nr_requests
```

### Network

```bash
# Increase socket buffers
sysctl -w net.core.rmem_max=16777216
sysctl -w net.core.wmem_max=16777216
sysctl -w net.ipv4.tcp_rmem="4096 87380 16777216"
sysctl -w net.ipv4.tcp_wmem="4096 65536 16777216"

# Enable TCP fast open
sysctl -w net.ipv4.tcp_fastopen=3
```

### File Descriptors

```bash
# Increase limits
cat >> /etc/security/limits.conf << EOF
kraft soft nofile 65536
kraft hard nofile 65536
EOF

# Verify
su - kraft -c "ulimit -n"
```

---

## Debug Tools

### Dump Internal State

```bash
# Dump raft state
kraft-ctl dump-state > state.json

# Contents:
# {
#   "node_id": 2,
#   "term": 42,
#   "role": "leader",
#   "commit_index": 1234567,
#   "last_applied": 1234567,
#   "voted_for": 2,
#   "log": {
#     "first_index": 1200000,
#     "last_index": 1234567,
#     "entries_count": 34568
#   },
#   "peers": [...]
# }
```

### Trace Requests

```bash
# Enable request tracing
kraft-ctl set-log-level debug

# Trace specific request
kraft-ctl trace-request --key="user:123"

# Output shows full request path through system
```

### Network Diagnostics

```bash
# Test connectivity to all peers
kraft-ctl ping-peers

# Output:
# node1: 1.2ms
# node3: 1.5ms
# node4: 1.1ms
# node5: 45.3ms  [SLOW]

# Capture raft traffic
tcpdump -i eth0 port 7001 -w raft.pcap
```

---

## Common Error Messages

### "not leader"

```
Error: not leader, leader is node 2

Cause: Request sent to follower
Solution: Client should redirect to indicated leader
```

### "term mismatch"

```
Error: term mismatch: local=42, remote=41

Cause: Stale node communicating with cluster
Solution: Node will automatically update term and step down
```

### "log mismatch"

```
Error: log mismatch at index 1000: expected term 5, got term 4

Cause: Log divergence (usually after partition heal)
Solution: Follower will automatically truncate and sync
```

### "snapshot too old"

```
Error: cannot install snapshot: index 500 < first log index 1000

Cause: Snapshot is older than current log start
Solution: Take new snapshot, or let node sync normally
```

### "quorum not reached"

```
Error: quorum not reached: got 2/5, need 3

Cause: Not enough nodes responding
Solution: Check node health and network connectivity
```

---

## Getting Help

### Collect Diagnostics

```bash
#!/bin/bash
# collect-diagnostics.sh

OUTPUT_DIR="/tmp/kraft-diagnostics-$(date +%Y%m%d-%H%M%S)"
mkdir -p $OUTPUT_DIR

# Cluster status
kraft-ctl status > $OUTPUT_DIR/status.txt
kraft-ctl nodes > $OUTPUT_DIR/nodes.txt

# Node state
kraft-ctl dump-state > $OUTPUT_DIR/state.json

# Recent logs
journalctl -u kraft -n 1000 > $OUTPUT_DIR/logs.txt

# System info
uname -a > $OUTPUT_DIR/system.txt
free -h >> $OUTPUT_DIR/system.txt
df -h >> $OUTPUT_DIR/system.txt
iostat -x >> $OUTPUT_DIR/system.txt

# Network info
netstat -tlnp | grep kraft >> $OUTPUT_DIR/network.txt
ss -s >> $OUTPUT_DIR/network.txt

# Create archive
tar czf $OUTPUT_DIR.tar.gz $OUTPUT_DIR
echo "Diagnostics collected: $OUTPUT_DIR.tar.gz"
```

### Contact Support

When reporting issues, include:

1. Kraft version (`kraft-server --version`)
2. Cluster size and configuration
3. Symptom description
4. Diagnostics archive
5. Steps to reproduce

---

## Next Steps

- [Monitoring](monitoring.md) - Set up monitoring
- [Deployment](deployment.md) - Production configuration
- [Configuration Reference](../reference/configuration.md) - All options
