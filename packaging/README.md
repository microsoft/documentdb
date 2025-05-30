# To Build Your Own Packages With Docker

## Building Debian/Ubuntu Packages

Run `./packaging/build_packages.sh -h` and follow the instructions.
E.g. to build for Debian 12 and PostgreSQL 16, run:

```sh
./packaging/build_packages.sh --os deb12 --pg 16
```

## Building RPM Packages

For Red Hat-based distributions, you can build RPM packages:

```sh
./packaging/build_packages.sh --os rhel8 --pg 17
```

Supported RPM distributions:
- rhel8 (Red Hat Enterprise Linux 8 compatible)
- rhel9 (Red Hat Enterprise Linux 9 compatible)

### Network Requirements for RPM Builds

**Important**: RPM builds require network access to external repositories during the Docker build process. The build may fail in environments with restrictive firewalls that block access to:

- Package repository mirrors (e.g., `mirrors.rockylinux.org`, `mirrorlist.centos.org`)
- PostgreSQL repository downloads
- External dependency sources

If you encounter network connectivity errors during RPM builds:

1. **Check firewall settings**: Ensure your build environment can access external package repositories
2. **Use a different network**: Try building from a network with fewer restrictions
3. **Contact your system administrator**: Request access to the blocked domains
4. **Alternative**: Use pre-built packages when available or build in an unrestricted environment

### RPM Build Prerequisites

Before building RPM packages, you can validate your environment:

```sh
./packaging/validate_rpm_build.sh
```

This script checks:
- Docker installation and availability
- Network connectivity for package repositories
- Access to required base images

### Example RPM Build Commands

```sh
# Build for RHEL 8 with PostgreSQL 17
./packaging/build_packages.sh --os rhel8 --pg 17

# Build for RHEL 9 with PostgreSQL 16
./packaging/build_packages.sh --os rhel9 --pg 16

# Build with testing enabled
./packaging/build_packages.sh --os rhel8 --pg 17 --test-clean-install
```

## Output

Packages can be found at the `packages` directory by default, but it can be configured with the `--output-dir` option.

**Note:** The packages do not include pg_documentdb_distributed in the `internal` directory.