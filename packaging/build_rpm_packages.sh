#!/bin/bash

set -euo pipefail

# Function to display help message
function show_help {
    echo "Usage: $0 --os <OS> --pg <PG_VERSION> [--version <VERSION>] [--test-clean-install] [--output-dir <DIR>] [-h|--help]"
    echo ""
    echo "Description:"
    echo "  This script builds RPM extension packages using Docker."
    echo ""
    echo "Mandatory Arguments:"
    echo "  --os                 OS to build packages for. Possible values: [rhel8, rhel9]"
    echo "  --pg                 PG version to build packages for. Possible values: [15, 16, 17]"
    echo ""
    echo "Optional Arguments:"
    echo "  --version            The version of documentdb to build. Examples: [0.100.0, 0.101.0]"
    echo "  --test-clean-install Test installing the packages in a clean Docker container."
    echo "  --output-dir         Relative path from the repo root of the directory where to drop the packages. The directory will be created if it doesn't exist. Default: packaging"
    echo "  -h, --help           Display this help message."
    exit 0
}

# Initialize variables
OS=""
PG=""
DOCUMENTDB_VERSION=""
TEST_CLEAN_INSTALL=false
OUTPUT_DIR="packaging"  # Default value for output directory

# Process arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --os)
            shift
            case $1 in
                rhel8|rhel9)
                    OS=$1
                    ;;
                *)
                    echo "Invalid --os value. Allowed values are [rhel8, rhel9]"
                    exit 1
                    ;;
            esac
            ;;
        --pg)
            shift
            case $1 in
                15|16|17)
                    PG=$1
                    ;;
                *)
                    echo "Invalid --pg value. Allowed values are [15, 16, 17]"
                    exit 1
                    ;;
            esac
            ;;
        --version)
            shift
            DOCUMENTDB_VERSION=$1
            ;;
        --test-clean-install)
            TEST_CLEAN_INSTALL=true
            ;;
        --output-dir)
            shift
            OUTPUT_DIR=$1
            ;;
        -h|--help)
            show_help
            ;;
        *)
            echo "Unknown argument: $1"
            show_help
            exit 1
            ;;
    esac
    shift
done

# Check mandatory arguments
if [[ -z "$OS" ]]; then
    echo "Error: --os is required."
    show_help
    exit 1
fi

if [[ -z "$PG" ]]; then
    echo "Error: --pg is required."
    show_help
    exit 1
fi

# get the version from control file
if [[ -z "$DOCUMENTDB_VERSION" ]]; then
    DOCUMENTDB_VERSION=$(grep -E "^default_version" pg_documentdb_core/documentdb_core.control | sed -E "s/.*'([0-9]+\.[0-9]+-[0-9]+)'.*/\1/")
    DOCUMENTDB_VERSION=$(echo $DOCUMENTDB_VERSION | sed "s/-/./g")
    echo "DOCUMENTDB_VERSION extracted from control file: $DOCUMENTDB_VERSION"
    if [[ -z "$DOCUMENTDB_VERSION" ]]; then
        echo "Error: --version is required and could not be found in the control file."
        show_help
        exit 1
    fi
fi

# Set the appropriate Docker image based on the OS
OS_VERSION_NUMBER=""
case $OS in
    rhel8)
        DOCKER_IMAGE="rockylinux:8"
        OS_VERSION_NUMBER="8"
        ;;
    rhel9)
        DOCKER_IMAGE="rockylinux:9"
        OS_VERSION_NUMBER="9"
        ;;
    *)
        # This case should not be reached due to earlier validation, but as a safeguard:
        echo "Error: Invalid OS specified for RPM build: $OS"
        exit 1
        ;;
esac

DOCKERFILE="packaging/Dockerfile_build_rpm_packages"
TAG=documentdb-build-packages-$OS-pg$PG:latest

repo_root=$(git rev-parse --show-toplevel)
abs_output_dir="$repo_root/$OUTPUT_DIR"
cd "$repo_root" # Ensure we are at the repo root for Docker build context

echo "Building RPM packages for OS: $OS, PostgreSQL version: $PG, DOCUMENTDB version: $DOCUMENTDB_VERSION"
echo "Output directory: $abs_output_dir"

# Create the output directory if it doesn't exist
mkdir -p "$abs_output_dir"

# Build the Docker image while showing the output to the console
docker build -t "$TAG" -f "$DOCKERFILE" \
    --build-arg BASE_IMAGE="$DOCKER_IMAGE" \
    --build-arg POSTGRES_VERSION="$PG" \
    --build-arg DOCUMENTDB_VERSION="$DOCUMENTDB_VERSION" \
    --build-arg OS_VERSION_ARG="$OS_VERSION_NUMBER" .

# Run the Docker container to build the packages
# Pass OS_VERSION_NUMBER as OS_VERSION to the container, as the Dockerfile_build_rpm_packages expects OS_VERSION
docker run --rm --env OS="$OS" --env POSTGRES_VERSION="$PG" -v "$abs_output_dir:/output" "$TAG"

echo "Packages built successfully!!"

if [[ $TEST_CLEAN_INSTALL == true ]]; then
    echo "Testing clean installation in a Docker container..."

    rpm_package_name=$(ls "$abs_output_dir" | grep -E "${OS}-postgresql${PG}-documentdb-${DOCUMENTDB_VERSION}.*\.x86_64\.rpm" | head -n 1)
    if [[ -z "$rpm_package_name" ]]; then
        echo "Error: Could not find the built RPM package in $abs_output_dir for testing."
        exit 1
    fi
    package_rel_path="$OUTPUT_DIR/$rpm_package_name"
    
    echo "RPM package path for testing: $package_rel_path"
    
    # Build the Docker image while showing the output to the console
    docker build -t documentdb-test-rpm-packages:latest -f packaging/test_packages/Dockerfile_test_install_rpm_packages \
        --build-arg BASE_IMAGE="$DOCKER_IMAGE" \
        --build-arg POSTGRES_VERSION="$PG" \
        --build-arg RPM_PACKAGE_REL_PATH="$package_rel_path" \
        --build-arg OS_VERSION_ARG="$OS_VERSION_NUMBER" .
        
    # Run the Docker container to test the packages
    docker run --rm --env POSTGRES_VERSION="$PG" documentdb-test-rpm-packages:latest
    
    echo "Clean installation test successful!!"
fi

echo "Packages are available in $abs_output_dir"