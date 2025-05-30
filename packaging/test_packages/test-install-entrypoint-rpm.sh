#!/bin/bash
set -e

echo "Testing RPM package installation..."

# Install the RPM package
yum install -y /tmp/documentdb.rpm

echo "RPM package installed successfully!"

# Initialize PostgreSQL data directory (if needed for testing)
# Note: This is a basic test to ensure the package installs without errors
# More comprehensive testing would require setting up a full PostgreSQL instance

echo "Package installation test completed successfully!"