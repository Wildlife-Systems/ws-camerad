#!/bin/bash
#
# Build script for camera daemon
#

set -e

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/usr/local}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
    exit 1
}

# Check dependencies
check_dependencies() {
    info "Checking dependencies..."
    
    local missing=()
    
    if ! command -v cmake &> /dev/null; then
        missing+=("cmake")
    fi
    
    if ! pkg-config --exists libcamera 2>/dev/null; then
        missing+=("libcamera-dev")
    fi
    
    if ! pkg-config --exists libjpeg 2>/dev/null; then
        missing+=("libjpeg-dev")
    fi
    
    if [ ${#missing[@]} -ne 0 ]; then
        error "Missing dependencies: ${missing[*]}\nInstall with: sudo apt install ${missing[*]}"
    fi
    
    info "All dependencies found"
}

# Build
build() {
    info "Building camera daemon..."
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    cmake .. \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
    
    make -j$(nproc)
    
    info "Build complete"
}

# Install
install() {
    info "Installing..."
    
    cd "$BUILD_DIR"
    sudo make install
    
    # Create directories
    sudo mkdir -p /var/lib/ws-camerad/stills
    sudo mkdir -p /var/lib/ws-camerad/clips
    sudo mkdir -p /etc/ws/camerad
    
    # Install config if not exists
    if [ ! -f /etc/ws/camerad/ws-camerad.conf ]; then
        sudo cp ../config/ws-camerad.conf /etc/ws/camerad/
    fi
    
    # Install systemd service
    sudo cp ../config/camera-daemon.service /etc/systemd/system/
    sudo systemctl daemon-reload
    
    info "Installation complete"
    info "Start with: sudo systemctl start camera-daemon"
}

# Clean
clean() {
    info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    info "Clean complete"
}

# Usage
usage() {
    echo "Usage: $0 [command]"
    echo ""
    echo "Commands:"
    echo "  deps      Check dependencies"
    echo "  build     Build the project (default)"
    echo "  install   Install to system"
    echo "  clean     Clean build directory"
    echo "  all       Build and install"
    echo ""
    echo "Environment variables:"
    echo "  BUILD_DIR      Build directory (default: build)"
    echo "  BUILD_TYPE     CMake build type (default: Release)"
    echo "  INSTALL_PREFIX Install prefix (default: /usr/local)"
}

# Main
case "${1:-build}" in
    deps)
        check_dependencies
        ;;
    build)
        check_dependencies
        build
        ;;
    install)
        install
        ;;
    clean)
        clean
        ;;
    all)
        check_dependencies
        build
        install
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        error "Unknown command: $1"
        ;;
esac
