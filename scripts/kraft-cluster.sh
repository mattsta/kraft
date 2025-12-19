#!/bin/bash
#
# kraft-cluster.sh - Cluster orchestration for kraft-loopy
#
# Commands:
#   start <n>           Start n-node cluster (default: 3)
#   stop                Stop all running nodes
#   status              Show cluster status
#   kill <node>         Kill specific node (1-indexed)
#   restart <node>      Restart specific node
#   pause <node>        Pause node (SIGSTOP)
#   resume <node>       Resume node (SIGCONT)
#   logs [node]         Show logs (all or specific node)
#   clean               Remove all data directories
#
# Environment:
#   KRAFT_BINARY        Path to kraft-loopy binary (default: ./kraft-loopy)
#   KRAFT_DATA_DIR      Base data directory (default: /tmp/kraft-cluster)
#   KRAFT_BASE_PORT     Base port number (default: 5001)
#   KRAFT_LOG_DIR       Log directory (default: $KRAFT_DATA_DIR/logs)
#

set -e

# Configuration
KRAFT_BINARY="${KRAFT_BINARY:-./kraft-loopy}"
KRAFT_DATA_DIR="${KRAFT_DATA_DIR:-/tmp/kraft-cluster}"
KRAFT_BASE_PORT="${KRAFT_BASE_PORT:-5001}"
KRAFT_LOG_DIR="${KRAFT_LOG_DIR:-$KRAFT_DATA_DIR/logs}"
KRAFT_PID_DIR="$KRAFT_DATA_DIR/pids"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Print colored output
info() { echo -e "${BLUE}[INFO]${NC} $*"; }
success() { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# Get node port
node_port() {
    local node=$1
    echo $((KRAFT_BASE_PORT + node - 1))
}

# Get node data directory
node_data_dir() {
    local node=$1
    echo "$KRAFT_DATA_DIR/node$node"
}

# Get node PID file
node_pid_file() {
    local node=$1
    echo "$KRAFT_PID_DIR/node$node.pid"
}

# Get node log file
node_log_file() {
    local node=$1
    echo "$KRAFT_LOG_DIR/node$node.log"
}

# Check if node is running
node_is_running() {
    local node=$1
    local pid_file
    pid_file=$(node_pid_file "$node")

    if [[ -f "$pid_file" ]]; then
        local pid
        pid=$(cat "$pid_file")
        if kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
    fi
    return 1
}

# Get node PID
node_pid() {
    local node=$1
    local pid_file
    pid_file=$(node_pid_file "$node")

    if [[ -f "$pid_file" ]]; then
        cat "$pid_file"
    fi
}

# Build peer list for a node
build_peers() {
    local total=$1
    local self=$2
    local peers=""

    for ((i = 1; i <= total; i++)); do
        if [[ $i -ne $self ]]; then
            local port
            port=$(node_port "$i")
            if [[ -n "$peers" ]]; then
                peers="$peers,"
            fi
            peers="${peers}127.0.0.1:$port"
        fi
    done

    echo "$peers"
}

# Build cluster address string
build_cluster_addrs() {
    local total=$1
    local addrs=""

    for ((i = 1; i <= total; i++)); do
        local port
        port=$(node_port "$i")
        if [[ -n "$addrs" ]]; then
            addrs="$addrs,"
        fi
        addrs="${addrs}127.0.0.1:$port"
    done

    echo "$addrs"
}

# Start a single node
start_node() {
    local node=$1
    local total=$2
    local cluster_addrs=${3:-}

    local port
    port=$(node_port "$node")
    local data_dir
    data_dir=$(node_data_dir "$node")
    local pid_file
    pid_file=$(node_pid_file "$node")
    local log_file
    log_file=$(node_log_file "$node")

    # Check if already running
    if node_is_running "$node"; then
        warn "Node $node already running (PID: $(node_pid "$node"))"
        return 0
    fi

    # Create directories
    mkdir -p "$data_dir" "$KRAFT_PID_DIR" "$KRAFT_LOG_DIR"

    # Build command - use static cluster configuration
    local cmd="$KRAFT_BINARY --listen 127.0.0.1:$port --data $data_dir --node $node"

    if [[ -n "$cluster_addrs" ]] && [[ $total -gt 1 ]]; then
        # Multi-node cluster with static membership
        cmd="$cmd --cluster $cluster_addrs"
    fi
    # Single node mode: just --listen is sufficient

    # Start node in background
    info "Starting node $node on port $port..."
    $cmd > "$log_file" 2>&1 &
    local pid=$!

    # Save PID
    echo "$pid" > "$pid_file"

    # Wait a moment for startup
    sleep 0.5

    # Verify it's running
    if kill -0 "$pid" 2>/dev/null; then
        success "Node $node started (PID: $pid)"
    else
        error "Node $node failed to start"
        cat "$log_file"
        return 1
    fi
}

# Stop a single node
stop_node() {
    local node=$1
    local signal=${2:-TERM}

    if ! node_is_running "$node"; then
        warn "Node $node not running"
        return 0
    fi

    local pid
    pid=$(node_pid "$node")
    local pid_file
    pid_file=$(node_pid_file "$node")

    info "Stopping node $node (PID: $pid)..."
    kill -"$signal" "$pid" 2>/dev/null || true

    # Wait for process to exit
    local timeout=10
    while kill -0 "$pid" 2>/dev/null && [[ $timeout -gt 0 ]]; do
        sleep 0.5
        timeout=$((timeout - 1))
    done

    if kill -0 "$pid" 2>/dev/null; then
        warn "Node $node didn't stop gracefully, forcing..."
        kill -9 "$pid" 2>/dev/null || true
    fi

    rm -f "$pid_file"
    success "Node $node stopped"
}

# Get number of running nodes
count_running_nodes() {
    local count=0
    for pid_file in "$KRAFT_PID_DIR"/node*.pid; do
        [[ -f "$pid_file" ]] || continue
        local pid
        pid=$(cat "$pid_file")
        if kill -0 "$pid" 2>/dev/null; then
            count=$((count + 1))
        fi
    done
    echo "$count"
}

# Get total configured nodes (from existing data dirs)
count_total_nodes() {
    local count=0
    for data_dir in "$KRAFT_DATA_DIR"/node*; do
        [[ -d "$data_dir" ]] || continue
        count=$((count + 1))
    done
    echo "$count"
}

# Command: start
cmd_start() {
    local num_nodes=${1:-3}

    if [[ $num_nodes -lt 1 ]]; then
        error "Need at least 1 node"
        exit 1
    fi

    if [[ ! -x "$KRAFT_BINARY" ]]; then
        # Try to find in common locations
        if [[ -x "./src/loopy-demo/kraft-loopy" ]]; then
            KRAFT_BINARY="./src/loopy-demo/kraft-loopy"
        elif [[ -x "../build2/src/loopy-demo/kraft-loopy" ]]; then
            KRAFT_BINARY="../build2/src/loopy-demo/kraft-loopy"
        else
            error "kraft-loopy binary not found. Set KRAFT_BINARY or build first."
            exit 1
        fi
    fi

    info "Starting $num_nodes-node cluster..."
    info "Binary: $KRAFT_BINARY"
    info "Data: $KRAFT_DATA_DIR"
    info "Ports: $(node_port 1)-$(node_port "$num_nodes")"

    # Build cluster address list for static configuration
    local cluster_addrs=""
    if [[ $num_nodes -gt 1 ]]; then
        cluster_addrs=$(build_cluster_addrs "$num_nodes")
        info "Cluster: $cluster_addrs"
    fi

    # Start all nodes with static cluster configuration
    for ((i = 1; i <= num_nodes; i++)); do
        start_node "$i" "$num_nodes" "$cluster_addrs"
        sleep 0.3
    done

    echo
    success "Cluster started with $num_nodes nodes"
    cmd_status
}

# Command: stop
cmd_stop() {
    info "Stopping all nodes..."

    for pid_file in "$KRAFT_PID_DIR"/node*.pid; do
        [[ -f "$pid_file" ]] || continue
        local node
        node=$(basename "$pid_file" .pid | sed 's/node//')
        stop_node "$node"
    done

    success "All nodes stopped"
}

# Command: status
cmd_status() {
    echo
    echo "=== Kraft Cluster Status ==="
    echo

    local running=0
    local stopped=0

    # Find all nodes (either from PIDs or data dirs)
    local nodes=()
    for pid_file in "$KRAFT_PID_DIR"/node*.pid; do
        [[ -f "$pid_file" ]] || continue
        local node
        node=$(basename "$pid_file" .pid | sed 's/node//')
        nodes+=("$node")
    done

    for data_dir in "$KRAFT_DATA_DIR"/node*; do
        [[ -d "$data_dir" ]] || continue
        local node
        node=$(basename "$data_dir" | sed 's/node//')
        # Add if not already in list
        local found=false
        for n in "${nodes[@]}"; do
            [[ "$n" == "$node" ]] && found=true && break
        done
        $found || nodes+=("$node")
    done

    # Sort nodes
    IFS=$'\n' sorted=($(sort -n <<<"${nodes[*]}")); unset IFS

    if [[ ${#sorted[@]} -eq 0 ]]; then
        echo "No nodes found"
        return
    fi

    printf "%-8s %-10s %-8s %-20s\n" "NODE" "STATUS" "PID" "ADDRESS"
    printf "%-8s %-10s %-8s %-20s\n" "----" "------" "---" "-------"

    for node in "${sorted[@]}"; do
        local status pid port
        port=$(node_port "$node")

        if node_is_running "$node"; then
            pid=$(node_pid "$node")
            status="${GREEN}RUNNING${NC}"
            running=$((running + 1))
        else
            pid="-"
            status="${RED}STOPPED${NC}"
            stopped=$((stopped + 1))
        fi

        printf "%-8s " "$node"
        echo -en "$status"
        printf "%*s" $((10 - 7)) ""  # Adjust for color codes
        printf "%-8s %-20s\n" "$pid" "127.0.0.1:$port"
    done

    echo
    echo "Running: $running, Stopped: $stopped"
}

# Command: kill
cmd_kill() {
    local node=$1

    if [[ -z "$node" ]]; then
        error "Usage: $0 kill <node>"
        exit 1
    fi

    if ! node_is_running "$node"; then
        error "Node $node not running"
        exit 1
    fi

    local pid
    pid=$(node_pid "$node")
    info "Killing node $node (PID: $pid) with SIGKILL..."
    kill -9 "$pid" 2>/dev/null || true
    rm -f "$(node_pid_file "$node")"
    success "Node $node killed"
}

# Command: restart
cmd_restart() {
    local node=$1

    if [[ -z "$node" ]]; then
        error "Usage: $0 restart <node>"
        exit 1
    fi

    # Stop if running
    if node_is_running "$node"; then
        stop_node "$node"
    fi

    # Get total nodes from existing data dirs
    local total
    total=$(count_total_nodes)
    [[ $total -eq 0 ]] && total=3

    # Build cluster addresses and restart node
    local cluster_addrs=""
    if [[ $total -gt 1 ]]; then
        cluster_addrs=$(build_cluster_addrs "$total")
    fi
    start_node "$node" "$total" "$cluster_addrs"
}

# Command: pause
cmd_pause() {
    local node=$1

    if [[ -z "$node" ]]; then
        error "Usage: $0 pause <node>"
        exit 1
    fi

    if ! node_is_running "$node"; then
        error "Node $node not running"
        exit 1
    fi

    local pid
    pid=$(node_pid "$node")
    info "Pausing node $node (PID: $pid)..."
    kill -STOP "$pid"
    success "Node $node paused"
}

# Command: resume
cmd_resume() {
    local node=$1

    if [[ -z "$node" ]]; then
        error "Usage: $0 resume <node>"
        exit 1
    fi

    if ! node_is_running "$node"; then
        error "Node $node not running"
        exit 1
    fi

    local pid
    pid=$(node_pid "$node")
    info "Resuming node $node (PID: $pid)..."
    kill -CONT "$pid"
    success "Node $node resumed"
}

# Command: logs
cmd_logs() {
    local node=$1

    if [[ -z "$node" ]]; then
        # Show all logs
        for log_file in "$KRAFT_LOG_DIR"/node*.log; do
            [[ -f "$log_file" ]] || continue
            local n
            n=$(basename "$log_file" .log)
            echo "=== $n ==="
            tail -20 "$log_file"
            echo
        done
    else
        local log_file
        log_file=$(node_log_file "$node")
        if [[ -f "$log_file" ]]; then
            tail -f "$log_file"
        else
            error "No log file for node $node"
            exit 1
        fi
    fi
}

# Command: clean
cmd_clean() {
    # Stop all nodes first
    cmd_stop

    info "Removing data directory: $KRAFT_DATA_DIR"
    rm -rf "$KRAFT_DATA_DIR"
    success "Cleaned"
}

# Main
main() {
    local cmd=${1:-help}
    shift || true

    case "$cmd" in
        start)
            cmd_start "$@"
            ;;
        stop)
            cmd_stop
            ;;
        status)
            cmd_status
            ;;
        kill)
            cmd_kill "$@"
            ;;
        restart)
            cmd_restart "$@"
            ;;
        pause)
            cmd_pause "$@"
            ;;
        resume)
            cmd_resume "$@"
            ;;
        logs)
            cmd_logs "$@"
            ;;
        clean)
            cmd_clean
            ;;
        help|--help|-h)
            echo "Usage: $0 <command> [args]"
            echo
            echo "Commands:"
            echo "  start <n>       Start n-node cluster (default: 3)"
            echo "  stop            Stop all running nodes"
            echo "  status          Show cluster status"
            echo "  kill <node>     Kill specific node (1-indexed)"
            echo "  restart <node>  Restart specific node"
            echo "  pause <node>    Pause node (SIGSTOP)"
            echo "  resume <node>   Resume node (SIGCONT)"
            echo "  logs [node]     Show logs (all or specific node)"
            echo "  clean           Remove all data directories"
            echo
            echo "Environment:"
            echo "  KRAFT_BINARY    Path to kraft-loopy (default: ./kraft-loopy)"
            echo "  KRAFT_DATA_DIR  Data directory (default: /tmp/kraft-cluster)"
            echo "  KRAFT_BASE_PORT Base port (default: 5001)"
            ;;
        *)
            error "Unknown command: $cmd"
            echo "Run '$0 help' for usage"
            exit 1
            ;;
    esac
}

main "$@"
