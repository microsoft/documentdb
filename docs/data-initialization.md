# Data Initialization Support

This file documents the data initialization feature in DocumentDB that allows users to initialize their database with custom data or built-in sample data.

## Environment Variables

The following environment variables are supported:

- `INIT_DATA_PATH`: Path to directory containing .js initialization files (default: /init_doc_db.d)
- `INIT_DATA`: Enable built-in sample data initialization (default: false)

## Command Line Options

- `--init-data-path [PATH]`: Specify directory containing JavaScript files for database initialization
- `--init-data`: Enable initialization with built-in sample data (flag, defaults to false)

## Usage Examples

### Using built-in sample data
```bash
docker run -p 10260:10260 -p 9712:9712 \
  --init-data \
  --password mypassword \
  documentdb/local
```

### Using custom initialization files
```bash
docker run -p 10260:10260 -p 9712:9712 \
  -v /path/to/your/init/scripts:/init_doc_db.d \
  --init-data-path /init_doc_db.d \
  --password mypassword \
  documentdb/local
```

### Using environment variables
```bash
docker run -p 10260:10260 -p 9712:9712 \
  -e INIT_DATA_PATH=/custom/init/path \
  -e INIT_DATA=true \
  -e PASSWORD=mypassword \
  -v /path/to/your/init/scripts:/custom/init/path \
  documentdb/local
```

## Built-in Sample Data

When using `--init-data` flag or `INIT_DATA=true`, the following sample collections are created in the 'sampledb' database:
- users (5 sample users)
- products (5 sample products)  
- orders (4 sample orders)
- analytics (sample metrics and activity data)

## Security Note

Built-in sample data is disabled by default. Custom initialization scripts can be provided for production use.
