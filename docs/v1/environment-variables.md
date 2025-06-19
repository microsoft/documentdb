# Arguments

The emulator allows arguments with docker run command. 

## Usage

```bash
# With Docker using environment variables
 docker run -dt -p 10260:10260 -e USERNAME=Username -e PASSWORD=YourPassword -e DATA_PATH=/custum_data_path -v documentdb-data:/custum_data_path ghcr.io/microsoft/documentdb/documentdb-local:latest
```


## Environment Variables

| Environment Variable | Type | Default | Description |
|---------------------|------|---------|-------------|
| `ENFORCE_SSL` | boolean | `true` | Enforce SSL for all connections |
| `CERT_PATH` | string | - | Path to SSL certificate file |
| `KEY_FILE` | string | - | Path to SSL private key file |
| `DATA_PATH` | string | `/data` | Data directory path |
| `DOCUMENTDB_PORT` | integer | `10260` | DocumentDB service port |
| `POSTGRESQL_PORT` | integer | `9712` | PostgreSQL server port |
| `USERNAME` | string | `default_user` | DocumentDB username |
| `PASSWORD` | string | `Admin100` | DocumentDB password |
| `CREATE_USER` | boolean | `true` | Whether to create a user |
| `START_POSTGRESQL` | boolean | `true` | Whether to start PostgreSQL server |
| `OWNER` | string | `documentdb` | Service owner |
| `ALLOW_EXTERNAL_CONNECTIONS` | boolean | `false` | Allow external PostgreSQL connections |
| `ENABLE_TELEMETRY` | boolean | - | Enable telemetry collection |
| `LOG_LEVEL` | string | `info` | Log verbosity: `quiet`, `error`, `warn`, `info`, `debug`, `trace` |

## Docker Examples

```bash
# Default startup
 docker run -dt -p 10260:10260 ghcr.io/microsoft/documentdb/documentdb-local:latest

# Custom ports
docker run -dt -p 10260:10260 -e DOCUMENTDB_PORT=27017 -e POSTGRESQL_PORT=5432 ghcr.io/microsoft/documentdb/documentdb-local:latest

# Custom username and password
 docker run -dt -p 10260:10260 -e USERNAME=Username -e PASSWORD=YourPassword ghcr.io/microsoft/documentdb/documentdb-local:latest

# Custom data path
 docker run -dt -p 10260:10260 -e DATA_PATH=/custum_data_path -v documentdb-data:/custum_data_path ghcr.io/microsoft/documentdb/documentdb-local:latest
```

## Certificate Management

- Both `CERT_PATH` and `KEY_FILE` must be provided together
- If not provided, a self-signed certificate is auto-generated
- Generated files: `$HOME/self_signed_cert.pem`, `$HOME/self_signed_key.pem`

## Log Files

- Gateway: `/home/documentdb/gateway.log`
- PostgreSQL: `/home/documentdb/oss_server.log`
- Startup: `/home/documentdb/gateway_entrypoint.log`
