# Release Workflow Documentation

This document describes the automated release workflow for DocumentDB.

## Overview

The automated release workflow creates comprehensive releases including:
- DEB packages for Ubuntu/Debian systems
- RPM packages for RHEL-compatible systems  
- Docker images with the packages
- GitHub releases with all artifacts
- Package signing and verification

## Workflow Triggers

### 1. Manual Trigger (Recommended)

Use GitHub Actions UI or API to manually trigger a release:

```bash
# Via GitHub CLI
gh workflow run release.yml -f version=v0.105-0 -f create_draft=true

# Via GitHub API
curl -X POST \
  -H "Authorization: token $GITHUB_TOKEN" \
  -H "Accept: application/vnd.github.v3+json" \
  https://api.github.com/repos/microsoft/documentdb/actions/workflows/release.yml/dispatches \
  -d '{"ref":"main","inputs":{"version":"v0.105-0","create_draft":"true"}}'
```

**Parameters:**
- `version`: Version to release (e.g., `v0.105-0`)
- `create_draft`: Whether to create a draft release (default: `true`)

### 2. Automatic Trigger

The workflow automatically triggers when you push a tag matching the pattern `v*`:

```bash
git tag v0.105-0
git push origin v0.105-0
```

## Release Process

### 1. Version and Changelog Extraction

The workflow:
- Extracts the version from the tag or manual input
- Parses `CHANGELOG.md` to extract release notes for the version
- Extracts the release date from the changelog

### 2. Package Building

**DEB Packages:**
- Builds for Ubuntu 22.04, 24.04, Debian 11, 12
- Supports amd64 and arm64 architectures
- Updates `packaging/debian_files/changelog` with release notes
- Signs packages with temporary GPG keys
- Creates artifacts for each OS/arch/PostgreSQL version combination

**RPM Packages:**
- Builds for RHEL 8, 9 compatible systems
- Supports amd64 architecture
- Updates `packaging/rpm_files/documentdb.spec` changelog section
- Signs packages with temporary GPG keys
- Tests clean installation in containers

### 3. Docker Image Building

- Uses DEB packages from Ubuntu 22.04 builds
- Creates images for PostgreSQL 16 and 17
- Supports amd64 and arm64 architectures
- Pushes to GitHub Container Registry (GHCR)
- Signs images with cosign (keyless signing)
- Tags with both version and `latest`

### 4. Release Creation

- Downloads all build artifacts
- Organizes packages and signing keys
- Generates SHA256 checksums
- Creates GitHub release with:
  - Release notes from CHANGELOG.md
  - All package files
  - Signing keys
  - Checksums
  - Installation instructions
  - Docker image information

## Release Artifacts

Each release includes:

### Packages
- DEB packages: `*-postgresql-{16,17}-documentdb_*.deb`
- RPM packages: `*-postgresql{16,17}-documentdb-*.rpm`
- Debug symbol packages (DEB only)

### Signing Keys
- `documentdb-signing-key.asc` (for DEB packages)
- `documentdb-rpm-signing-key.asc` (for RPM packages)

### Checksums
- `SHA256SUMS` - SHA256 hashes for all packages and keys

### Docker Images
Images are available at:
```
ghcr.io/microsoft/documentdb/documentdb-oss:PG{16,17}-{amd64,arm64}-{version}
ghcr.io/microsoft/documentdb/documentdb-oss:PG{16,17}-{amd64,arm64}-latest
```

## Package Verification

### DEB Package Verification

```bash
# Import signing key
wget https://github.com/microsoft/documentdb/releases/download/v0.105-0/documentdb-signing-key.asc
gpg --import documentdb-signing-key.asc

# Verify package signature
dpkg-sig --verify package.deb
```

### RPM Package Verification

```bash
# Import signing key
wget https://github.com/microsoft/documentdb/releases/download/v0.105-0/documentdb-rpm-signing-key.asc
rpm --import documentdb-rpm-signing-key.asc

# Verify package signature
rpm --checksig package.rpm
```

### Docker Image Verification

```bash
# Verify image signature with cosign
cosign verify \
  --certificate-identity-regexp "https://github.com/microsoft/documentdb/.github/workflows/release.yml@.*" \
  --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
  ghcr.io/microsoft/documentdb/documentdb-oss:PG16-amd64-v0.105-0
```

## Prerequisites

### For Maintainers

- Push access to the repository
- Ability to create tags
- Access to GitHub Actions

### For Release Process

- Docker installed and running on GitHub Actions runners
- Network access to package repositories
- Permissions to write to GitHub Container Registry

## Troubleshooting

### Common Issues

1. **Build Failures**
   - Check Docker daemon status
   - Verify network connectivity to package repositories
   - Ensure base images are accessible

2. **Signing Failures**
   - Check GPG key generation
   - Verify signing tools installation
   - Ensure proper permissions

3. **Upload Failures**
   - Check GitHub token permissions
   - Verify repository access
   - Check artifact size limits

### Debug Steps

1. Check workflow logs in GitHub Actions
2. Verify CHANGELOG.md format for the version
3. Ensure version exists in `CHANGELOG.md`
4. Check Docker daemon on self-hosted runners
5. Verify network connectivity

### Manual Recovery

If automated release fails:

1. **Manual Package Build:**
   ```bash
   ./packaging/build_packages.sh --os ubuntu22.04 --pg 16 --version 0.105.0
   ```

2. **Manual Docker Build:**
   ```bash
   docker build -t documentdb:test -f .github/containers/Build-Ubuntu/Dockerfile_prebuild .
   ```

3. **Manual Release Creation:**
   Use GitHub UI to create release and upload artifacts manually

## Security Considerations

### Package Signing

- Packages are signed with temporary keys generated during build
- In production, use secure key management (Azure Key Vault, AWS KMS, etc.)
- Keys are included in release for verification purposes
- Consider using organization-level signing keys for production

### Image Signing

- Docker images are signed with cosign using keyless signing
- Signatures are verifiable against GitHub OIDC provider
- Images are scanned for vulnerabilities (if configured)

### Dependency Security

- Base images are pulled from official repositories
- Package dependencies are verified through official repositories
- Build environment is isolated in containers

## Maintenance

### Regular Tasks

1. **Update Base Images**
   - Monitor for security updates
   - Update Dockerfile base image references
   - Test builds with new base images

2. **Update Dependencies**
   - Monitor PostgreSQL version updates
   - Update supported PostgreSQL versions in matrix
   - Test compatibility with new versions

3. **Review Signing Keys**
   - Rotate temporary signing keys if needed
   - Consider implementing persistent signing infrastructure
   - Update key expiration dates

### Version Updates

When adding new PostgreSQL versions or OS targets:

1. Update the matrix in `.github/workflows/release.yml`
2. Test builds for new combinations
3. Update documentation
4. Verify package dependencies are available

## Related Files

- `.github/workflows/release.yml` - Main release workflow
- `.github/workflows/build_packages.yml` - DEB package building
- `.github/workflows/build_rpm_packages.yml` - RPM package building
- `packaging/build_packages.sh` - Package build script
- `packaging/debian_files/changelog` - Debian package changelog
- `packaging/rpm_files/documentdb.spec` - RPM package specification
- `CHANGELOG.md` - Project changelog and release notes source