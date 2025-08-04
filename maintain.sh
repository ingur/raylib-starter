#!/bin/bash
set -euo pipefail

# --- Configuration ---

readonly PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly DOCKER_IMAGE="raylib-starter-maintenance"
readonly DOCKERFILE="Dockerfile.maintenance"

# --- Utility Functions ---

log_info() {
    echo -e "\033[1;36m[INFO]\033[0m $@"
}

log_success() {
    echo -e "\033[1;32m[SUCCESS]\033[0m $@"
}

log_error() {
    echo -e "\033[1;31m[ERROR]\033[0m $@"
}

check_docker() {
    if ! command -v docker &> /dev/null; then
        log_error "Docker is not installed"
        exit 1
    fi
    
    if ! docker info &> /dev/null; then
        log_error "Docker is not running or you don't have permission to access it"
        exit 1
    fi
}

ensure_maintenance_branch() {
    local current_branch=$(git branch --show-current)
    if [ "$current_branch" != "maintenance" ]; then
        log_info "Switching to maintenance branch"
        git checkout maintenance
    fi
}

ensure_docker_image() {
    if ! docker images | grep -q "$DOCKER_IMAGE"; then
        log_info "Building maintenance Docker image"
        docker build -f "$DOCKERFILE" -t "$DOCKER_IMAGE" .
    fi
}

# --- Maintenance Functions ---

sync() {
    log_info "Syncing with main branch"
    ensure_maintenance_branch
    
    git fetch origin main
    git merge origin/main -m "Sync with main" --no-edit
    
    log_success "Synced with main"
}

update() {
    local library=$1
    
    [ "$library" != "miniz" ] && [ "$library" != "luajit" ] && [ "$library" != "raylib" ] && [ "$library" != "all" ] && \
        log_error "Invalid library: $library" && exit 1
    
    log_info "Updating $library"
    check_docker
    ensure_maintenance_branch
    ensure_docker_image
    
    docker run --rm -v "$PROJECT_ROOT:/workspace" "$DOCKER_IMAGE" ./scripts/update-$library.sh
    
    # Check if there are changes
    if ! git diff --quiet; then
        log_success "Libraries updated. Review changes with 'git diff' before committing."
        git status --short
    else
        log_info "No changes - libraries are already up to date"
    fi
}

# --- Command Handling ---

show_help() {
    echo "Usage: $0 <command> [options]"
    echo "Commands:"
    echo "  sync                    Sync maintenance branch with main"
    echo "  update <library>        Update libraries (miniz|luajit|raylib|all)"
    echo "  help                    Show this help message"
}

if [ $# -eq 0 ]; then
    show_help
    exit 1
fi

case $1 in
    sync)
        sync
        ;;
    update)
        [ $# -lt 2 ] && log_error "Missing library name" && show_help && exit 1
        update "$2"
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        log_error "Unknown command: $1"
        show_help
        exit 1
        ;;
esac

exit 0