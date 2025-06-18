## Get Started

### Pre-requisite

- Ensure [Docker](https://docs.docker.com/engine/install/) is installed on your system.

### Building DocumentDB with Docker

Step 1: Clone the DocumentDB repo.

```bash
git clone https://github.com/microsoft/documentdb.git
```

Step 2: Create the docker image. Navigate to cloned repo.

```bash
docker build . -f .devcontainer/Dockerfile -t documentdb 
```

Note: Validate using `docker image ls`

Step 3: Run the Image as a container

```bash
docker run -v $(pwd):/home/documentdb/code -it documentdb /bin/bash 

cd code
```

(Aligns local location with docker image created, allows de-duplicating cloning repo again within image).<br>
Note: Validate container is running `docker container ls`

Step 4: Build & Deploy the binaries

```bash
make 
```

Note: Run in case of an unsuccessful build `git config --global --add safe.directory /home/documentdb/code` within image.

```bash
sudo make install
```

Note: To run backend postgresql tests after installing you can run `make check`.

You are all set to work with DocumentDB.

### Using the Prebuilt Docker Image

You can use a [prebuilt docker image](https://github.com/microsoft/documentdb/pkgs/container/documentdb%2Fdocumentdb-oss/versions?filters%5Bversion_type%5D=tagged) for DocumentDB instead of building it from source.  Follow these steps:

#### Pull the Prebuilt Image

Pull the prebuilt image directly from the Microsoft Container Registry:

```bash
docker pull ghcr.io/microsoft/documentdb/documentdb-oss:PG16-amd64-0.105.0
```

#### Running the Prebuilt Image

To run the prebuilt image, use one of the following commands:

1. Run the container:

```bash
docker run -dt ghcr.io/microsoft/documentdb/documentdb-oss:PG16-amd64-0.105.0
```

2. If external access is required, run the container with parameter "-e":

```bash
docker run -p 127.0.0.1:9712:9712 -dt ghcr.io/microsoft/documentdb/documentdb-oss:PG16-amd64-0.105.0 -e
```

This will start the container and map port `9712` from the container to the host.

### Connecting to the Server
#### Internal Access
Step 1: Run `start_oss_server.sh` to initialize the DocumentDB server and manage dependencies.

```bash
./scripts/start_oss_server.sh
```

Or logging into the container if using prebuild image
```bash
docker exec -it <container-id> bash
```

Step 2: Connect to `psql` shell

```bash
psql -p 9712 -d postgres
```

#### External Access
Connect to `psql` shell

```bash
psql -h localhost --port 9712 -d postgres -U documentdb
```