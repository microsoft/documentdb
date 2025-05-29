#!/bin/bash
set -e

test -n "$OS" || (echo "OS not set" && false)

# Change to the build directory
cd /build

# Keep the internal directory out of the RPM package
sed -i '/internal/d' Makefile

# Create RPM build directories
mkdir -p ~/rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

# Copy spec file to the appropriate location
cp rpm_files/documentdb.spec ~/rpmbuild/SPECS/

# Create source tarball
tar --exclude='.git' --exclude='internal' --exclude='packaging' -czf ~/rpmbuild/SOURCES/documentdb-${DOCUMENTDB_VERSION}.tar.gz -C .. $(basename $(pwd))

# Build the RPM package
rpmbuild -ba ~/rpmbuild/SPECS/documentdb.spec

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