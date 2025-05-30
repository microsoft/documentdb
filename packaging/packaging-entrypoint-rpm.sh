#!/bin/bash
set -e

test -n "$OS" || (echo "OS not set" && false)

# Change to the build directory
cd /build

# Keep the internal directory out of the RPM package
sed -i '/internal/d' Makefile

# Create RPM build directories
mkdir -p ~/rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

# Detect the available PostgreSQL version in the build environment
if [ -d "/usr/lib/postgresql/" ]; then
    AVAILABLE_PG_VERSION=$(ls /usr/lib/postgresql/ | head -1)
    echo "Detected PostgreSQL version: $AVAILABLE_PG_VERSION"
    export PG_CONFIG="/usr/lib/postgresql/$AVAILABLE_PG_VERSION/bin/pg_config"
else
    echo "PostgreSQL not found, using default version"
    AVAILABLE_PG_VERSION="14"
fi

# Get the PostgreSQL version from the environment or use detected version
PG_VERSION=${POSTGRES_VERSION:-$AVAILABLE_PG_VERSION}

# Get the package version from the spec file
PACKAGE_VERSION=$(grep "^Version:" rpm_files/documentdb.spec | awk '{print $2}')

# Construct the actual package name (after variable substitution)
PACKAGE_NAME="postgresql${PG_VERSION}-documentdb"

echo "Package name: $PACKAGE_NAME"
echo "Package version: $PACKAGE_VERSION"
echo "PostgreSQL version: $PG_VERSION"

# Copy spec file to the appropriate location
cp rpm_files/documentdb.spec ~/rpmbuild/SPECS/

# Create source tarball with the expected name
echo "Creating tarball: ~/rpmbuild/SOURCES/${PACKAGE_NAME}-${PACKAGE_VERSION}.tar.gz"
# Create a temporary directory with the expected name and copy source files there
temp_source_dir="/tmp/${PACKAGE_NAME}-${PACKAGE_VERSION}"
mkdir -p "$temp_source_dir"
cp -r * "$temp_source_dir/" 2>/dev/null || true
# Exclude unwanted directories
rm -rf "$temp_source_dir/.git" "$temp_source_dir/internal" "$temp_source_dir/packaging" 2>/dev/null || true
tar -czf ~/rpmbuild/SOURCES/${PACKAGE_NAME}-${PACKAGE_VERSION}.tar.gz -C /tmp ${PACKAGE_NAME}-${PACKAGE_VERSION}
rm -rf "$temp_source_dir"

# Build the RPM package
# Since we're cross-compiling from Ubuntu, skip dependency checks
rpmbuild -ba --nodeps ~/rpmbuild/SPECS/documentdb.spec

# Change to the RPM output directory
cd ~/rpmbuild/RPMS/x86_64

# Rename .rpm files to include the OS name prefix
for f in *.rpm; do
   mv "$f" "${OS}-${f}";
done

# Create the output directory if it doesn't exist
mkdir -p /output

# Copy the built packages to the output directory
cp *.rpm /output/

# Also copy source RPMs if they exist
if [ -d ~/rpmbuild/SRPMS ] && [ "$(ls -A ~/rpmbuild/SRPMS)" ]; then
    cd ~/rpmbuild/SRPMS
    for f in *.rpm; do
       mv "$f" "${OS}-${f}";
    done
    cp *.rpm /output/
fi

# Change ownership of the output files to match the host user's UID and GID
chown -R $(stat -c "%u:%g" /output) /output