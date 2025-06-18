#!/bin/bash
set -e

# Ensure required environment variables are set
test -n "$OS" || (echo "OS not set" && false)
test -n "$POSTGRES_VERSION" || (echo "POSTGRES_VERSION not set" && false)

# Change to the build directory
cd /build

# Remove 'internal' references from Makefile
sed -i '/internal/d' Makefile

# Create RPM build directories
mkdir -p ~/rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

# Get the package version from the spec file
PACKAGE_VERSION=$(grep "^Version:" rpm_files/documentdb.spec | awk '{print $2}')

# Construct the package name
PACKAGE_NAME="postgresql${POSTGRES_VERSION}-documentdb"

# Add PostgreSQL bin directory to PATH to ensure pg_config is found
export PATH="/usr/pgsql-${POSTGRES_VERSION}/bin:$PATH"
echo "Using PostgreSQL bin directory: $PATH"

echo "Package name: $PACKAGE_NAME"
echo "Package version: $PACKAGE_VERSION"
echo "PostgreSQL version: $POSTGRES_VERSION"

# Copy spec file to the SPECS directory
cp rpm_files/documentdb.spec ~/rpmbuild/SPECS/

# Prepare the source directory
SOURCE_DIR="/tmp/${PACKAGE_NAME}-${PACKAGE_VERSION}"
mkdir -p "$SOURCE_DIR"

# Copy source files into the source directory
# Adjust this as needed to include all necessary files
cp -r /build/* "$SOURCE_DIR/"

# Create the source tarball
echo "Creating tarball: ~/rpmbuild/SOURCES/${PACKAGE_NAME}-${PACKAGE_VERSION}.tar.gz"
tar -czf ~/rpmbuild/SOURCES/${PACKAGE_NAME}-${PACKAGE_VERSION}.tar.gz -C /tmp "${PACKAGE_NAME}-${PACKAGE_VERSION}"

# Build the RPM package
rpmbuild -ba ~/rpmbuild/SPECS/documentdb.spec

# Rename and copy RPMs to the output directory
mkdir -p /output
for rpm_file in ~/rpmbuild/RPMS/x86_64/*.rpm; do
    base_rpm=$(basename "$rpm_file")
    mv "$rpm_file" "/output/${OS}-${base_rpm}"
done

# Also handle source RPMs
# if [ -d ~/rpmbuild/SRPMS ]; then
#     for srpm_file in ~/rpmbuild/SRPMS/*.rpm; do
#         base_srpm=$(basename "$srpm_file")
#         mv "$srpm_file" "/output/${OS}-${base_srpm}"
#     done
# fi

# Adjust ownership of the output files
chown -R $(stat -c "%u:%g" /output) /output
