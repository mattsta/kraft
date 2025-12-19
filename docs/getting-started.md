# Getting Started with Kraft

This guide will help you build and run your first Kraft cluster in under 10 minutes.

## Prerequisites

- C compiler (GCC 7+ or Clang 8+)
- CMake 3.10+
- POSIX-compatible system (Linux, macOS, BSD)
- Optional: libuv for production networking

## Building Kraft

```bash
# Clone the repository
git clone https://github.com/mattsta/kraft.git
cd kraft

# Create build directory
mkdir build && cd build

# Configure and build
# uh, note: currently the deps are missing but they are all public and be manually added back.
# TBD: add deps auto-download again.
cmake ..
make -j32

# Run tests to verify
./src/kraft-test
```

### Build Options

| Option              | Description         | Default |
| ------------------- | ------------------- | ------- |
| `KRAFT_BUILD_TESTS` | Build test suite    | ON      |
| `KRAFT_VERBOSE`     | Enable debug output | OFF     |
| `CMAKE_BUILD_TYPE`  | Debug/Release       | Release |

```bash
# Debug build with verbose output
cmake -DCMAKE_BUILD_TYPE=Debug -DKRAFT_VERBOSE=ON ..
```

---

## Your First Client

Create a simple client that connects to a Kraft cluster and submits commands.

### Step 1: Include Headers

```c
#include "kraftClientAPI.h"
#include "kraftAdapterLibuv.h"
#include "kraftProtocol.h"

#include <stdio.h>
#include <string.h>
```

### Step 2: Create Network Components

```c
int main(void) {
    // Create the network adapter (handles I/O, timers)
    kraftAdapter *adapter = kraftAdapterLibuvCreate();
    if (!adapter) {
        fprintf(stderr, "Failed to create adapter\n");
        return 1;
    }

    // Create the protocol encoder/decoder
    kraftProtocol *protocol = kraftProtocolCreate(
        kraftProtocolGetBinaryOps(), NULL);
    if (!protocol) {
        fprintf(stderr, "Failed to create protocol\n");
        return 1;
    }

    // Create the client session
    kraftClientSession *client = kraftClientCreate(adapter, protocol, NULL);
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }
```

### Step 3: Configure Cluster Endpoints

```c
    // Add cluster node addresses
    kraftClientAddEndpoint(client, "127.0.0.1", 7001);
    kraftClientAddEndpoint(client, "127.0.0.1", 7002);
    kraftClientAddEndpoint(client, "127.0.0.1", 7003);
```

### Step 4: Connect and Submit Commands

```c
    // Connect to cluster
    if (!kraftClientConnect(client)) {
        fprintf(stderr, "Failed to connect\n");
        return 1;
    }

    // Submit a command
    const char *command = "SET mykey myvalue";
    kraftClientSubmit(client,
                      (uint8_t *)command,
                      strlen(command),
                      NULL,   // callback (NULL = fire-and-forget)
                      NULL);  // user data

    // Run event loop to process the request
    kraftClientRun(client);
```

### Step 5: Cleanup

```c
    // Cleanup
    kraftClientDestroy(client);
    kraftProtocolDestroy(protocol);
    kraftAdapterDestroy(adapter);

    return 0;
}
```

### Complete Example

```c
// file: my_client.c
#include "kraftClientAPI.h"
#include "kraftAdapterLibuv.h"
#include "kraftProtocol.h"
#include <stdio.h>
#include <string.h>

void onResponse(kraftClientSession *session,
                const kraftClientRequest *request,
                const kraftClientResponse *response,
                void *userData) {
    if (response->status == KRAFT_CLIENT_OK) {
        printf("Command committed at index %llu\n",
               (unsigned long long)response->logIndex);
    } else {
        printf("Command failed: %s\n",
               kraftClientStatusString(response->status));
    }

    // Stop the event loop after receiving response
    kraftClientStop(session);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command>\n", argv[0]);
        return 1;
    }

    // Setup
    kraftAdapter *adapter = kraftAdapterLibuvCreate();
    kraftProtocol *protocol = kraftProtocolCreate(
        kraftProtocolGetBinaryOps(), NULL);
    kraftClientSession *client = kraftClientCreate(adapter, protocol, NULL);

    // Configure
    kraftClientAddEndpoint(client, "127.0.0.1", 7001);
    kraftClientAddEndpoint(client, "127.0.0.1", 7002);
    kraftClientAddEndpoint(client, "127.0.0.1", 7003);

    // Connect and submit
    kraftClientConnect(client);
    kraftClientSubmit(client,
                      (uint8_t *)argv[1],
                      strlen(argv[1]),
                      onResponse,
                      NULL);

    // Run until response received
    kraftClientRun(client);

    // Cleanup
    kraftClientDestroy(client);
    kraftProtocolDestroy(protocol);
    kraftAdapterDestroy(adapter);

    return 0;
}
```

### Compile and Run

```bash
# Compile (adjust include/library paths as needed)
gcc -o my_client my_client.c \
    -I/path/to/kraft/src \
    -L/path/to/kraft/build/src \
    -lkraft -luv -lm -lpthread

# Run
./my_client "SET user:1 {name: 'Alice'}"
```

---

## Your First Server

Create a Kraft server node that participates in a cluster.

### Complete Server Example

```c
// file: my_server.c
#include "kraft.h"
#include "kraftNetwork.h"
#include "kraftAdapterLibuv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Called when commands are committed
void applyCommand(const void *data, uint64_t len) {
    printf("APPLY: %.*s\n", (int)len, (const char *)data);
    // Your state machine logic here
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <node_id> <port>\n", argv[0]);
        return 1;
    }

    uint64_t nodeId = atoi(argv[1]);
    uint16_t port = atoi(argv[2]);

    // Create Raft state
    kraftState *state = kraftStateNew();

    // Configure cluster (3 nodes)
    kraftIp *peers[3];
    kraftIp peer1 = {.ip = "127.0.0.1", .port = 7001, .outbound = true};
    kraftIp peer2 = {.ip = "127.0.0.1", .port = 7002, .outbound = true};
    kraftIp peer3 = {.ip = "127.0.0.1", .port = 7003, .outbound = true};
    peers[0] = &peer1;
    peers[1] = &peer2;
    peers[2] = &peer3;

    // Initialize Raft
    kraftStateInitRaft(state, 3, peers, 0x12345678, nodeId);

    // Set command handler
    state->internals.data.apply = applyCommand;

    // Create network components
    kraftAdapter *adapter = kraftAdapterLibuvCreate();
    kraftProtocol *protocol = kraftProtocolCreate(
        kraftProtocolGetBinaryOps(), NULL);

    // Create and configure server
    kraftServer *server = kraftServerCreate(adapter, protocol);
    kraftServerSetState(server, state);
    kraftServerListenOn(server, "0.0.0.0", port);

    printf("Node %llu listening on port %u\n",
           (unsigned long long)nodeId, port);

    // Run (blocks)
    kraftServerRun(server);

    // Cleanup
    kraftServerDestroy(server);
    kraftStateFree(state);

    return 0;
}
```

### Running a 3-Node Cluster

Open three terminals and run:

```bash
# Terminal 1
./my_server 1 7001

# Terminal 2
./my_server 2 7002

# Terminal 3
./my_server 3 7003
```

You'll see election messages as the cluster elects a leader.

---

## Next Steps

- [Architecture Overview](architecture.md) - Understand how Kraft works
- [Client Development Guide](guides/client-development.md) - Advanced client patterns
- [Server Development Guide](guides/server-development.md) - Production server setup
- [Multi-Raft Guide](concepts/multi-raft.md) - Scale with multiple Raft groups
