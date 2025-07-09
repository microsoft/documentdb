# Custom Data Initialization Support

This file documents the data initialization feature in DocumentDB that allows users to initialize their database with custom data.

## Environment Variables

The following environment variable is supported:

- `INIT_DATA_PATH`: Path to directory containing .js initialization files (default: /docker-entrypoint-initdb.d)

## Usage Examples

### Using custom initialization files
```bash
docker run -p 10260:10260 -p 9712:9712 \
  -v /path/to/your/init/scripts:/docker-entrypoint-initdb.d \
  --init-data-path /docker-entrypoint-initdb.d \
  --password mypassword \
  documentdb/local
```

### Using environment variables
```bash
docker run -p 10260:10260 -p 9712:9712 \
  -e INIT_DATA_PATH=/custom/init/path \
  -e PASSWORD=mypassword \
  -v /path/to/your/init/scripts:/custom/init/path \
  documentdb/local
```

## Security Note

This feature only supports user-provided initialization scripts. No built-in sample data is included to ensure production safety.
