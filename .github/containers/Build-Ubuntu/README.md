# To Build Ubuntu prebuild image

E.g. to build for Ubuntu 22.04, PostgreSQL 16, amd64 and documentdb_0.103.0, run:

```sh
docker build -t <iamge-tag> -f .github/containers/Build-Ubuntu/Dockerfile_prebuild \ 
    --build-arg BASE_IMAGE=ubuntu:22.04 --build-arg POSTGRES_VERSION=16 \ 
    --build-arg DEB_PACKAGE_REL_PATH=packaging/packages/ubuntu22.04-postgresql-16-documentdb_0.103.0_amd64.deb .
```
