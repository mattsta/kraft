# Kraft Documentation

**Kraft** is a production-ready Raft consensus implementation in C with pluggable networking, featuring multi-raft groups, CRDT-aware compaction, intelligent snapshots, and comprehensive chaos testing.

## Documentation Index

### Getting Started

| Document                          | Description                                  |
| --------------------------------- | -------------------------------------------- |
| [Quick Start](getting-started.md) | Build your first Kraft cluster in 10 minutes |
| [Architecture](architecture.md)   | System design and component overview         |
| [Building](building.md)           | Compilation, dependencies, and configuration |

### Core Concepts

| Document                                                 | Description                              |
| -------------------------------------------------------- | ---------------------------------------- |
| [Raft Fundamentals](concepts/raft-fundamentals.md)       | Leader election, log replication, safety |
| [Pluggable Networking](concepts/pluggable-networking.md) | Adapter and protocol abstractions        |
| [Multi-Raft](concepts/multi-raft.md)                     | Running multiple Raft groups             |

### Implementation Guides

| Document                                           | Description                          |
| -------------------------------------------------- | ------------------------------------ |
| [Client Development](guides/client-development.md) | Building applications that use Kraft |
| [Server Development](guides/server-development.md) | Running Kraft cluster nodes          |
| [Custom Adapters](guides/custom-adapters.md)       | Implementing event loop integrations |
| [Custom Protocols](guides/custom-protocols.md)     | Implementing wire format encodings   |

### Advanced Features

| Document                                               | Description                           |
| ------------------------------------------------------ | ------------------------------------- |
| [Snapshots & Compaction](features/snapshots.md)        | Log compaction and state snapshots    |
| [CRDT Support](features/crdt.md)                       | Conflict-free replicated data types   |
| [Witness Nodes](features/witnesses.md)                 | Non-voting read replicas              |
| [Async Commit Modes](features/async-commit.md)         | Durability vs latency tradeoffs       |
| [Leadership Transfer](features/leadership-transfer.md) | Graceful leader handoff               |
| [ReadIndex Protocol](features/read-index.md)           | Linearizable reads without log writes |

### Operations

| Document                                          | Description                             |
| ------------------------------------------------- | --------------------------------------- |
| [Production Deployment](operations/deployment.md) | Cluster sizing, configuration, security |
| [Monitoring](operations/monitoring.md)            | Metrics, observability, alerting        |
| [Troubleshooting](operations/troubleshooting.md)  | Common issues and solutions             |

### Reference

| Document                                    | Description                |
| ------------------------------------------- | -------------------------- |
| [API Reference](reference/api.md)           | Complete API documentation |
| [Configuration](reference/configuration.md) | All configuration options  |
| [Message Types](reference/messages.md)      | Raft RPC message formats   |

---

## Quick Links

```c
// Minimal client example
#include "kraftClientAPI.h"
#include "kraftAdapterLibuv.h"

int main(void) {
    kraftAdapter *adapter = kraftAdapterLibuvCreate();
    kraftProtocol *protocol = kraftProtocolCreate(kraftProtocolGetBinaryOps(), NULL);
    kraftClientSession *client = kraftClientCreate(adapter, protocol, NULL);

    kraftClientAddEndpoint(client, "node1.cluster", 7001);
    kraftClientConnect(client);

    kraftClientSubmit(client, (uint8_t *)"SET key value", 13, NULL, NULL);
    kraftClientRun(client);

    return 0;
}
```

## License

See [LICENSE](../LICENSE) for details.
