#!/bin/bash
set -e

echo "Testing RPM package installation..."

# Install the RPM package
dnf install -y /tmp/documentdb.rpm

echo "RPM package installed successfully!"

cd /usr/src/documentdb

# Ensure the documentdb user has permissions to run tests in the extension directory
adduser --system --no-create-home documentdb
chown -R documentdb:documentdb .

# Switch to the documentdb user and run the tests
# su documentdb -c "make check"

echo "Package installation test completed successfully!"