#!/bin/bash
#
# kraft-tmux.sh - Tmux-based multi-node cluster runner
#
# Starts a kraft cluster with each node in its own tmux pane for
# interactive debugging and visual monitoring.
#
# Usage:
#   kraft-tmux.sh start <n>     Start n-node cluster in tmux
#   kraft-tmux.sh attach        Attach to existing session
#   kraft-tmux.sh kill          Kill the tmux session
#
# Environment:
#   KRAFT_BINARY        Path to kraft-loopy binary
#   KRAFT_DATA_DIR      Base data directory
#   KRAFT_BASE_PORT     Base port number (default: 5001)
#   KRAFT_SESSION       Tmux session name (default: kraft-cluster)
#

set -e

# Configuration
KRAFT_BINARY="${KRAFT_BINARY:-./kraft-loopy}"
KRAFT_DATA_DIR="${KRAFT_DATA_DIR:-/tmp/kraft-cluster}"
KRAFT_BASE_PORT="${KRAFT_BASE_PORT:-5001}"
KRAFT_SESSION="${KRAFT_SESSION:-kraft-cluster}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info() { echo -e "${BLUE}[INFO]${NC} $*"; }
success() { echo -e "${GREEN}[OK]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# Check if tmux is available
check_tmux() {
    if ! command -v tmux &> /dev/null; then
        error "tmux is required but not installed"
        exit 1
    fi
}

# Find kraft-loopy binary
find_binary() {
    if [[ -x "$KRAFT_BINARY" ]]; then
        return 0
    fi

    # Try common locations
    local candidates=(
        "./kraft-loopy"
        "./src/loopy-demo/kraft-loopy"
        "../build2/src/loopy-demo/kraft-loopy"
        "build/src/loopy-demo/kraft-loopy"
        "build2/src/loopy-demo/kraft-loopy"
    )

    for c in "${candidates[@]}"; do
        if [[ -x "$c" ]]; then
            KRAFT_BINARY="$c"
            return 0
        fi
    done

    error "kraft-loopy binary not found"
    error "Set KRAFT_BINARY or build with: make kraft-loopy"
    exit 1
}

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

# Calculate tmux layout
# Returns: rows cols
calculate_layout() {
    local n=$1

    if [[ $n -le 2 ]]; then
        echo "1 $n"
    elif [[ $n -le 4 ]]; then
        echo "2 2"
    elif [[ $n -le 6 ]]; then
        echo "2 3"
    elif [[ $n -le 9 ]]; then
        echo "3 3"
    elif [[ $n -le 12 ]]; then
        echo "3 4"
    elif [[ $n -le 16 ]]; then
        echo "4 4"
    elif [[ $n -le 20 ]]; then
        echo "4 5"
    else
        # For larger clusters, calculate approximate grid
        local cols=$(( (n + 4) / 5 ))
        local rows=$(( (n + cols - 1) / cols ))
        echo "$rows $cols"
    fi
}

# Command: start
cmd_start() {
    local num_nodes=${1:-3}

    check_tmux
    find_binary

    if [[ $num_nodes -lt 1 ]]; then
        error "Need at least 1 node"
        exit 1
    fi

    # Kill existing session if any
    tmux kill-session -t "$KRAFT_SESSION" 2>/dev/null || true

    info "Starting $num_nodes-node cluster in tmux session '$KRAFT_SESSION'..."
    info "Binary: $KRAFT_BINARY"
    info "Data: $KRAFT_DATA_DIR"
    info "Ports: $(node_port 1)-$(node_port "$num_nodes")"

    # Create directories
    for ((i = 1; i <= num_nodes; i++)); do
        mkdir -p "$(node_data_dir "$i")"
    done

    # Build cluster addresses for static configuration
    local cluster_addrs=""
    if [[ $num_nodes -gt 1 ]]; then
        cluster_addrs=$(build_cluster_addrs "$num_nodes")
        info "Cluster: $cluster_addrs"
    fi

    # Create new tmux session with first node
    local port
    port=$(node_port 1)
    local data_dir
    data_dir=$(node_data_dir 1)
    local cmd="$KRAFT_BINARY --listen 127.0.0.1:$port --data $data_dir --node 1"
    if [[ -n "$cluster_addrs" ]]; then
        cmd="$cmd --cluster $cluster_addrs"
    fi

    # Add a title and the command
    local pane_cmd="printf '\\033]2;Node 1\\033\\\\'; echo '=== Node 1 on port $port ==='; $cmd; echo 'Node 1 exited. Press Enter to close.'; read"

    tmux new-session -d -s "$KRAFT_SESSION" -x 200 -y 50 "$pane_cmd"

    # Add remaining nodes in new panes
    for ((i = 2; i <= num_nodes; i++)); do
        port=$(node_port "$i")
        data_dir=$(node_data_dir "$i")
        cmd="$KRAFT_BINARY --listen 127.0.0.1:$port --data $data_dir --node $i"
        if [[ -n "$cluster_addrs" ]]; then
            cmd="$cmd --cluster $cluster_addrs"
        fi

        pane_cmd="printf '\\033]2;Node $i\\033\\\\'; echo '=== Node $i on port $port ==='; sleep 0.2; $cmd; echo 'Node $i exited. Press Enter to close.'; read"

        # Split window and run node
        tmux split-window -t "$KRAFT_SESSION" "$pane_cmd"

        # Rebalance panes
        tmux select-layout -t "$KRAFT_SESSION" tiled
    done

    # Final layout adjustment
    tmux select-layout -t "$KRAFT_SESSION" tiled

    # Select first pane
    tmux select-pane -t "$KRAFT_SESSION":0.0

    success "Cluster started in tmux session '$KRAFT_SESSION'"
    echo
    echo "Commands:"
    echo "  Attach:  tmux attach -t $KRAFT_SESSION"
    echo "  Detach:  Ctrl-b d"
    echo "  Kill:    $0 kill"
    echo
    echo "Tmux navigation:"
    echo "  Next pane:     Ctrl-b o"
    echo "  Select pane:   Ctrl-b q <number>"
    echo "  Zoom pane:     Ctrl-b z"
    echo

    # Optionally attach immediately
    if [[ -t 0 ]]; then
        read -p "Attach to session now? [Y/n] " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]] || [[ -z $REPLY ]]; then
            tmux attach -t "$KRAFT_SESSION"
        fi
    fi
}

# Command: attach
cmd_attach() {
    check_tmux

    if ! tmux has-session -t "$KRAFT_SESSION" 2>/dev/null; then
        error "No tmux session '$KRAFT_SESSION' found"
        error "Start a cluster first: $0 start <n>"
        exit 1
    fi

    tmux attach -t "$KRAFT_SESSION"
}

# Command: kill
cmd_kill() {
    check_tmux

    if tmux has-session -t "$KRAFT_SESSION" 2>/dev/null; then
        info "Killing tmux session '$KRAFT_SESSION'..."
        tmux kill-session -t "$KRAFT_SESSION"
        success "Session killed"
    else
        info "No session '$KRAFT_SESSION' to kill"
    fi
}

# Command: send
cmd_send() {
    local node=$1
    shift
    local cmd="$*"

    check_tmux

    if [[ -z "$node" ]] || [[ -z "$cmd" ]]; then
        error "Usage: $0 send <node> <command>"
        exit 1
    fi

    if ! tmux has-session -t "$KRAFT_SESSION" 2>/dev/null; then
        error "No tmux session found"
        exit 1
    fi

    # Panes are 0-indexed
    local pane=$((node - 1))

    tmux send-keys -t "$KRAFT_SESSION":0.$pane "$cmd" Enter
}

# Command: status
cmd_status() {
    check_tmux

    if ! tmux has-session -t "$KRAFT_SESSION" 2>/dev/null; then
        echo "No tmux session '$KRAFT_SESSION' running"
        exit 0
    fi

    echo "=== Tmux Session: $KRAFT_SESSION ==="
    echo
    tmux list-panes -t "$KRAFT_SESSION" -F "Pane #{pane_index}: #{pane_current_command} (#{pane_width}x#{pane_height})"
}

# Main
main() {
    local cmd=${1:-help}
    shift || true

    case "$cmd" in
        start)
            cmd_start "$@"
            ;;
        attach|a)
            cmd_attach
            ;;
        kill|stop)
            cmd_kill
            ;;
        send)
            cmd_send "$@"
            ;;
        status)
            cmd_status
            ;;
        help|--help|-h)
            echo "Usage: $0 <command> [args]"
            echo
            echo "Commands:"
            echo "  start <n>           Start n-node cluster in tmux (default: 3)"
            echo "  attach              Attach to existing tmux session"
            echo "  kill                Kill the tmux session and all nodes"
            echo "  status              Show tmux session status"
            echo "  send <node> <cmd>   Send command to node's pane"
            echo
            echo "Environment:"
            echo "  KRAFT_BINARY    Path to kraft-loopy"
            echo "  KRAFT_DATA_DIR  Data directory (default: /tmp/kraft-cluster)"
            echo "  KRAFT_BASE_PORT Base port (default: 5001)"
            echo "  KRAFT_SESSION   Tmux session name (default: kraft-cluster)"
            echo
            echo "Tmux shortcuts (after attaching):"
            echo "  Ctrl-b d        Detach from session"
            echo "  Ctrl-b o        Next pane"
            echo "  Ctrl-b q        Show pane numbers"
            echo "  Ctrl-b z        Toggle pane zoom"
            echo "  Ctrl-b [        Scroll mode (q to exit)"
            ;;
        *)
            error "Unknown command: $cmd"
            echo "Run '$0 help' for usage"
            exit 1
            ;;
    esac
}

main "$@"
