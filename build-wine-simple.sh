#!/bin/bash

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

# Configuration
WINE_SOURCE_DIR="$(pwd)"
BUILD64_DIR="${WINE_SOURCE_DIR}/build64"
BUILD32_DIR="${WINE_SOURCE_DIR}/build32"
NUM_JOBS=$(nproc)

# Toolchain selection (gcc or clang)
MINGW_TOOLCHAIN="${MINGW_TOOLCHAIN:-gcc}"  # Default to gcc, can be set to "clang"

# V4L2 options
V4L2_OPTION="--with-v4l2"

# GPHOTO options
GPHOTO_OPTION="--with-gphoto"

# CUPS options
CUPS_OPTION="--with-cups"

print_status "Wine source directory: ${WINE_SOURCE_DIR}"
print_status "Build directories: ${BUILD64_DIR} and ${BUILD32_DIR}"
print_status "Using ${NUM_JOBS} parallel jobs"
print_status "MinGW toolchain: ${MINGW_TOOLCHAIN}"

# Function to check if ccache is configured
check_ccache() {
    if ! command -v ccache &> /dev/null; then
        print_error "ccache is not installed. Please install it first: sudo pacman -S ccache"
        exit 1
    fi

    # Configure ccache if not already done
    if [[ ! -f ~/.ccache/ccache.conf ]]; then
        print_status "Configuring ccache..."
        mkdir -p ~/.ccache
        ccache -M 5G > /dev/null
        print_success "ccache configured with 5GB cache size"
    fi

    # Show ccache statistics
    print_status "Current ccache statistics:"
    ccache -s
}

# Function to get cross-compiler commands based on toolchain
get_cross_compiler() {
    local arch="$1"
    case "${MINGW_TOOLCHAIN}" in
        clang)
            case "$arch" in
                i386) echo "ccache i686-w64-mingw32-clang" ;;
                x86_64) echo "ccache x86_64-w64-mingw32-clang" ;;
                *) echo "ccache ${arch}-w64-mingw32-clang" ;;
            esac
            ;;
        gcc|*)
            case "$arch" in
                i386) echo "ccache i686-w64-mingw32-gcc" ;;
                x86_64) echo "ccache x86_64-w64-mingw32-gcc" ;;
                *) echo "ccache ${arch}-w64-mingw32-gcc" ;;
            esac
            ;;
    esac
}


# Function to check for cross-compilers
check_cross_compilers() {
    case "${MINGW_TOOLCHAIN}" in
        clang)
            if ! command -v i686-w64-mingw32-clang &> /dev/null; then
                print_warning "32-bit Clang cross-compiler (i686-w64-mingw32-clang) not found"
                print_warning "32-bit PE compilation may fail"
            fi

            if ! command -v x86_64-w64-mingw32-clang &> /dev/null; then
                print_warning "64-bit Clang cross-compiler (x86_64-w64-mingw32-clang) not found"
                print_warning "64-bit PE compilation may fail"
            fi
            ;;
        gcc|*)
            if ! command -v i686-w64-mingw32-gcc &> /dev/null; then
                print_warning "32-bit GCC cross-compiler (i686-w64-mingw32-gcc) not found"
                print_warning "32-bit PE compilation may fail"
            fi

            if ! command -v x86_64-w64-mingw32-gcc &> /dev/null; then
                print_warning "64-bit GCC cross-compiler (x86_64-w64-mingw32-gcc) not found"
                print_warning "64-bit PE compilation may fail"
            fi
            ;;
    esac
}

# Function to check for v4l2 dependencies
check_v4l2_dependencies() {
    local arch="$1"

    if [[ "$arch" == "32bit" ]]; then
        # Check for 32-bit v4l2 development files
        if ! PKG_CONFIG_PATH=/usr/lib32/pkgconfig pkg-config --exists libv4l2 2>/dev/null || \
           [[ ! -f "/usr/lib32/pkgconfig/libv4l2.pc" ]]; then
            print_warning "32-bit libv4l2 development files not found"
            print_info "You can install them with: sudo pacman -S lib32-v4l-utils"
            print_info "Alternatively, use --skip-v4l2 to disable V4L2 support"
            return 1
        fi
    else
        # Check for 64-bit v4l2 development files
        if ! pkg-config --exists libv4l2 2>/dev/null; then
            print_warning "64-bit libv4l2 development files not found"
            print_info "You can install them with: sudo pacman -S v4l-utils"
            print_info "Alternatively, use --skip-v4l2 to disable V4L2 support"
            return 1
        fi
    fi

    return 0
}

# Function to check for gphoto2 dependencies
check_gphoto2_dependencies() {
    local arch="$1"

    if [[ "$arch" == "32bit" ]]; then
        # Check for 32-bit gphoto2 development files
        if ! PKG_CONFIG_PATH=/usr/lib32/pkgconfig pkg-config --exists libgphoto2 2>/dev/null || \
           [[ ! -f "/usr/lib32/pkgconfig/libgphoto2.pc" ]]; then
            print_warning "32-bit libgphoto2 development files not found"
            print_info "32-bit gphoto2 is not available in Arch repositories"
            return 1
        fi
    else
        # Check for 64-bit gphoto2 development files
        if ! pkg-config --exists libgphoto2 2>/dev/null; then
            print_warning "64-bit libgphoto2 development files not found"
            print_info "You can install them with: sudo pacman -S libgphoto2"
            return 1
        fi
    fi

    return 0
}

# Function to check for CUPS dependencies
check_cups_dependencies() {
    local arch="$1"

    if [[ "$arch" == "32bit" ]]; then
        # Check for 32-bit CUPS development files
        if ! PKG_CONFIG_PATH=/usr/lib32/pkgconfig pkg-config --exists cups 2>/dev/null || \
           [[ ! -f "/usr/lib32/pkgconfig/cups.pc" ]]; then
            print_warning "32-bit CUPS development files not found"
            print_info "32-bit CUPS development files are not available in Arch repositories"
            return 1
        fi
    else
        # Check for 64-bit CUPS development files
        if ! pkg-config --exists cups 2>/dev/null; then
            print_warning "64-bit CUPS development files not found"
            print_info "You can install them with: sudo pacman -S cups"
            return 1
        fi
    fi

    return 0
}

# Function to build 64-bit Wine
build_64bit() {
    print_status "Building 64-bit Wine..."

    # Create and enter build directory
    mkdir -p "${BUILD64_DIR}"
    cd "${BUILD64_DIR}"

    if [[ -f "config.status" ]]; then
        print_status "64-bit build already configured, skipping configure step"
    else
        # Configure 64-bit build
        print_status "Configuring 64-bit Wine build..."
        local x86_64_cc
        x86_64_cc="$(get_cross_compiler x86_64)"
        if ! ../configure \
            --enable-win64 \
            CC="ccache gcc" \
            CXX="ccache g++" \
            x86_64_CC="${x86_64_cc}" \
            --with-x \
            --with-gstreamer \
            --with-alsa \
            --with-pulse \
            --with-dbus \
            ${CUPS_OPTION} \
            ${SANE_OPTION} \
            ${GPHOTO_OPTION} \
            ${V4L2_OPTION} \
            --with-opencl \
            --with-vulkan \
            --with-sdl \
            ${OPENCL_OPTION}; then
            print_error "64-bit configuration failed"
            exit 1
        fi
    fi

    # Build 64-bit Wine
    print_status "Building 64-bit Wine with ${NUM_JOBS} jobs..."
    if ! make -j${NUM_JOBS}; then
        print_error "64-bit compilation failed"
        exit 1
    fi

    print_success "64-bit Wine build completed"
}

# Function to build 32-bit WoW64 Wine
build_32bit() {
    print_status "Building 32-bit WoW64 Wine..."

    # Create and enter build directory
    mkdir -p "${BUILD32_DIR}"
    cd "${BUILD32_DIR}"

    if [[ -f "config.status" ]]; then
        print_status "32-bit WoW64 build already configured, skipping configure step"
    else
        # Configure 32-bit WoW64 build
        print_status "Configuring 32-bit WoW64 Wine build..."
        local i386_cc
        i386_cc="$(get_cross_compiler i386)"
        if ! ../configure \
            --with-wine64="../build64" \
            CC="ccache gcc -m32" \
            CXX="ccache g++ -m32" \
            i386_CC="${i386_cc}" \
            --with-x \
            --with-gstreamer \
            --with-alsa \
            --with-pulse \
            --with-dbus \
            ${CUPS_OPTION} \
            ${SANE_OPTION} \
            ${GPHOTO_OPTION} \
            ${V4L2_OPTION} \
            --with-opencl \
            --with-vulkan \
            --with-sdl \
            ${OPENCL_OPTION}; then
            print_error "32-bit configuration failed"
            exit 1
        fi
    fi

    # Build 32-bit WoW64 Wine
    print_status "Building 32-bit WoW64 Wine with ${NUM_JOBS} jobs..."
    if ! make -j${NUM_JOBS}; then
        print_error "32-bit compilation failed"
        exit 1
    fi

    print_success "32-bit WoW64 Wine build completed"
}

# Function to verify the builds
verify_builds() {
    print_status "Verifying builds..."

    # Check if wine binaries exist
    if [[ -f "${BUILD64_DIR}/wine" ]] && [[ -f "${BUILD32_DIR}/wine" ]]; then
        print_success "Both 64-bit and 32-bit Wine builds are complete"

        # Show wine versions
        print_status "64-bit Wine version:"
        "${BUILD64_DIR}/wine" --version || true

        print_status "32-bit Wine version:"
        "${BUILD32_DIR}/wine" --version || true

    else
        print_error "Build verification failed - wine binaries not found"
        exit 1
    fi
}

# Function to show usage
show_usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -h, --help          Show this help message"
    echo "  -64, --64bit-only   Only build 64-bit Wine"
    echo "  -32, --32bit-only   Only build 32-bit Wine (requires existing 64-bit build)"
    echo "  -c, --clean         Clean build directories before building"
    echo "  -j N                Use N parallel jobs for building (default: auto-detected)"
    echo "  --skip-opencl       Disable OpenCL support (if dependencies are missing)"
    echo "  --skip-sane         Disable SANE scanner support (32-bit builds only)"
    echo "  --skip-v4l2         Disable V4L2 support"
    echo "  --skip-gphoto       Disable gphoto2 support"
    echo "  --skip-cups         Disable CUPS support"
    echo ""
    echo "Environment Variables:"
    echo "  MINGW_TOOLCHAIN     Set to 'gcc' (default) or 'clang' for MinGW toolchain"
    echo ""
    echo "Examples:"
    echo "  $0                  # Full mixed build with GCC"
    echo "  MINGW_TOOLCHAIN=clang $0  # Build with Clang MinGW"
    echo "  $0 -64              # Only build 64-bit"
    echo "  $0 -32              # Only build 32-bit (requires build64)"
    echo "  $0 -c               # Clean and rebuild"
    echo "  $0 -j 8             # Build with 8 parallel jobs"
    echo "  $0 --skip-opencl    # Build without OpenCL support"
    echo "  $0 --skip-v4l2      # Build without V4L2 support"
    echo "  $0 --skip-gphoto    # Build without gphoto2 support"
    echo "  $0 --skip-cups      # Build without CUPS support"
}

# Function to clean build directories
clean_build_dirs() {
    print_status "Cleaning build directories..."

    if [[ -d "${BUILD64_DIR}" ]]; then
        rm -rf "${BUILD64_DIR}"
        print_status "Removed ${BUILD64_DIR}"
    fi

    if [[ -d "${BUILD32_DIR}" ]]; then
        rm -rf "${BUILD32_DIR}"
        print_status "Removed ${BUILD32_DIR}"
    fi

    mkdir -p "${BUILD64_DIR}" "${BUILD32_DIR}"
    print_success "Build directories cleaned"
}

# Parse command line arguments
BUILD_64BIT=true
BUILD_32BIT=true
CLEAN_BUILD=false
SKIP_OPENCL=false
SKIP_SANE=false
SKIP_V4L2=false
SKIP_GPHOTO=false
SKIP_CUPS=false

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_usage
            exit 0
            ;;
        -64|--64bit-only)
            BUILD_64BIT=true
            BUILD_32BIT=false
            shift
            ;;
        -32|--32bit-only)
            BUILD_64BIT=false
            BUILD_32BIT=true
            shift
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        --skip-opencl)
            SKIP_OPENCL=true
            shift
            ;;
        --skip-sane)
            SKIP_SANE=true
            shift
            ;;
        --skip-v4l2)
            SKIP_V4L2=true
            shift
            ;;
        --skip-gphoto)
            SKIP_GPHOTO=true
            shift
            ;;
        --skip-cups)
            SKIP_CUPS=true
            shift
            ;;
        -j)
            NUM_JOBS="$2"
            shift 2
            ;;
        *)
            print_error "Unknown option: $1"
            show_usage
            exit 1
            ;;
    esac
done

# Main execution
main() {
    print_status "Starting Wine build process..."

    # Check if we're in the Wine source directory
    if [[ ! -f "configure.ac" ]] && [[ ! -f "configure" ]]; then
        print_error "This script must be run from the Wine source directory"
        exit 1
    fi

    # Check for cross-compilers
    check_cross_compilers

    # Show toolchain info
    print_status "Using ${MINGW_TOOLCHAIN} MinGW toolchain"
    print_status "32-bit cross-compiler: $(get_cross_compiler i386)"
    print_status "64-bit cross-compiler: $(get_cross_compiler x86_64)"

    # Handle OpenCL option
    if [[ "$SKIP_OPENCL" == true ]]; then
        OPENCL_OPTION="--without-opencl"
        print_status "OpenCL support disabled (--skip-opencl)"
    else
        OPENCL_OPTION="--with-opencl"
        print_status "OpenCL support enabled"
    fi

    # Handle V4L2 option
    if [[ "$SKIP_V4L2" == true ]]; then
        V4L2_OPTION="--without-v4l2"
        print_status "V4L2 support disabled (--skip-v4l2)"
    else
        # Check if we're building 32-bit and v4l2 dependencies are missing
        if [[ "$BUILD_32BIT" == true ]]; then
            if ! check_v4l2_dependencies "32bit"; then
                print_warning "Disabling V4L2 support for 32-bit build due to missing dependencies"
                V4L2_OPTION="--without-v4l2"
            else
                V4L2_OPTION="--with-v4l2"
                print_status "V4L2 support enabled for both 32-bit and 64-bit builds"
            fi
        else
            # Only building 64-bit, check 64-bit dependencies
            if ! check_v4l2_dependencies "64bit"; then
                print_warning "Disabling V4L2 support for 64-bit build due to missing dependencies"
                V4L2_OPTION="--without-v4l2"
            else
                V4L2_OPTION="--with-v4l2"
                print_status "V4L2 support enabled for 64-bit build"
            fi
        fi
    fi

    # Handle GPHOTO option
    if [[ "$SKIP_GPHOTO" == true ]]; then
        GPHOTO_OPTION="--without-gphoto"
        print_status "gphoto2 support disabled (--skip-gphoto)"
    else
        # Check if we're building 32-bit and gphoto2 dependencies are missing
        if [[ "$BUILD_32BIT" == true ]]; then
            if ! check_gphoto2_dependencies "32bit"; then
                print_warning "Disabling gphoto2 support for 32-bit build due to missing dependencies"
                GPHOTO_OPTION="--without-gphoto"
            else
                GPHOTO_OPTION="--with-gphoto"
                print_status "gphoto2 support enabled for both 32-bit and 64-bit builds"
            fi
        else
            # Only building 64-bit, check 64-bit dependencies
            if ! check_gphoto2_dependencies "64bit"; then
                print_warning "Disabling gphoto2 support for 64-bit build due to missing dependencies"
                GPHOTO_OPTION="--without-gphoto"
            else
                GPHOTO_OPTION="--with-gphoto"
                print_status "gphoto2 support enabled for 64-bit build"
            fi
        fi
    fi

    # Handle CUPS option
    if [[ "$SKIP_CUPS" == true ]]; then
        CUPS_OPTION="--without-cups"
        print_status "CUPS support disabled (--skip-cups)"
    else
        # Check if we're building 32-bit and CUPS dependencies are missing
        if [[ "$BUILD_32BIT" == true ]]; then
            if ! check_cups_dependencies "32bit"; then
                print_warning "Disabling CUPS support for 32-bit build due to missing dependencies"
                CUPS_OPTION="--without-cups"
            else
                CUPS_OPTION="--with-cups"
                print_status "CUPS support enabled for both 32-bit and 64-bit builds"
            fi
        else
            # Only building 64-bit, check 64-bit dependencies
            if ! check_cups_dependencies "64bit"; then
                print_warning "Disabling CUPS support for 64-bit build due to missing dependencies"
                CUPS_OPTION="--without-cups"
            else
                CUPS_OPTION="--with-cups"
                print_status "CUPS support enabled for 64-bit build"
            fi
        fi
    fi

    # Handle SANE option
    if [[ "$SKIP_SANE" == true ]]; then
        SANE_OPTION="--without-sane"
        print_status "SANE scanner support disabled (--skip-sane)"
    else
        # Check if 32-bit SANE is available - if not, disable it automatically
        if [[ "$BUILD_32BIT" == true ]] && ! pkg-config --exists sane 2>/dev/null; then
            SANE_OPTION="--without-sane"
            print_status "SANE scanner support disabled (32-bit SANE not available)"
        else
            SANE_OPTION="--with-sane"
            print_status "SANE scanner support enabled"
        fi
    fi

    # Configure ccache
    check_ccache

    # Clean build directories if requested
    if [[ "$CLEAN_BUILD" == true ]]; then
        clean_build_dirs
    fi

    # Build 64-bit if requested
    if [[ "$BUILD_64BIT" == true ]]; then
        build_64bit
    else
        print_status "Skipping 64-bit build (--32bit-only mode)"
    fi

    # Build 32-bit if requested
    if [[ "$BUILD_32BIT" == true ]]; then
        if [[ "$BUILD_64BIT" == false ]] && [[ ! -d "${BUILD64_DIR}" ]]; then
            print_error "64-bit build directory not found. Build 64-bit Wine first or use full build."
            exit 1
        fi
        build_32bit
    else
        print_status "Skipping 32-bit build (--64bit-only mode)"
    fi

    # Verify the builds if both were built
    if [[ "$BUILD_64BIT" == true ]] && [[ "$BUILD_32BIT" == true ]]; then
        verify_builds
        print_success "Mixed Wine build completed successfully!"
        print_status "You can now use:"
        print_status "  ${BUILD32_DIR}/wine   # For 32-bit applications (with WoW64 support)"
        print_status "  ${BUILD64_DIR}/wine   # For 64-bit applications"
    else
        print_success "Build completed successfully!"
    fi
}

# Run main function
main "$@"
