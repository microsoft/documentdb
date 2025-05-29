#!/bin/bash
# Quick validation script for RPM build environment

set -e

echo "DocumentDB RPM Build Validation Script"
echo "====================================="

# Check Docker availability
if ! command -v docker &> /dev/null; then
    echo "❌ Docker is not installed or not in PATH"
    exit 1
fi

echo "✓ Docker is available"

# Check if Docker is running
if ! docker info &> /dev/null; then
    echo "❌ Docker is not running"
    exit 1
fi

echo "✓ Docker is running"

# Test if we can pull the base image
echo "Testing base image availability..."
if docker pull quay.io/centos/centos:stream8 &> /dev/null; then
    echo "✓ CentOS Stream 8 base image is accessible"
else
    echo "❌ Cannot pull CentOS Stream 8 base image"
    exit 1
fi

echo ""
echo "✓ RPM build environment validation passed!"
echo ""
echo "You can now run RPM builds with:"
echo "  ./packaging/build_packages.sh --os rhel8 --pg 17"
echo "  ./packaging/build_packages.sh --os rhel9 --pg 16"