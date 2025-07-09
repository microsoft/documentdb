# DocumentDB Init-Data-Path Feature Test

This test suite validates the init-data-path feature of the DocumentDB gateway, which allows automatic initialization of database collections with sample data when the container starts.

## Overview

The test performs the following operations:
1. **Build**: Builds the DocumentDB gateway Docker image
2. **Run**: Starts the container with initialization data mounted
3. **Verify**: Uses mongosh to verify that all data was loaded correctly
4. **Report**: Outputs comprehensive test results

## File Structure

```
tests/test-init-data/
├── README.md                 # This documentation
├── test_init_data.sh         # Main test script
├── test-init-data/           # Initialization data files
│   ├── 01-users.js          # Creates users collection with 4 users
│   ├── 02-products.js       # Creates products collection with 4 products
│   └── 03-orders.js         # Creates orders collection with 4 orders
└── test-init-scripts/        # Alternative test scripts (if needed)
    ├── 01-create-users.js
    ├── 02-create-products.js
    └── 03-create-orders.js
```

## Prerequisites

- Docker installed and running
- MongoDB Shell (mongosh) installed and available in PATH
  - Install from: https://docs.mongodb.com/mongodb-shell/install/
- Port 10260 available on localhost

## Usage

### Quick Start
```bash
# Run the complete test suite
./test_init_data.sh
```

### What the Test Does

1. **Docker Image Build**
   - Builds the DocumentDB gateway image using the project's Dockerfile
   - Uses the correct build context and paths

2. **Container Initialization**
   - Starts a new container with the test data mounted at `/docker-entrypoint-initdb.d`
   - Passes the `--init-data-path` parameter to enable automatic initialization
   - Waits for the DocumentDB service to be ready

3. **Data Verification**
   - Uses mongosh from the host system to connect to the DocumentDB instance
   - Verifies all collections were created with the expected number of documents
   - Checks that all indexes were created properly
   - Tests basic queries to ensure the data is accessible
   - Validates specific query results (users > 30, products in stock, etc.)

4. **Test Results**
   - Displays a comprehensive test results table
   - Shows sample documents from each collection
   - Lists all created indexes
   - Provides overall pass/fail status
   - Automatically stops the container when testing is complete

## Expected Results

| Test Case | Expected | Description |
|-----------|----------|-------------|
| Users Collection | 4 | User accounts with profiles and preferences |
| Products Collection | 4 | Product catalog with specifications and pricing |
| Orders Collection | 4 | Order history with customer and item details |
| Adult Users | 3 | Users with age > 30 |
| Products In Stock | 2 | Products with inStock: true |
| Completed Orders | 1 | Orders with status: "completed" |

## Sample Data

### Users
- 4 users with different profiles, ages, and preferences
- Includes email index for fast lookups

### Products
- 4 products across different categories (Electronics, Food & Beverage, Home & Garden)
- Includes category, price, and stock status indexes

### Orders
- 4 orders with different statuses (completed, shipped, processing, cancelled)
- Includes userId, status, orderDate, and unique orderNumber indexes

## Troubleshooting

### Container fails to start
- Check if port 10260 is available
- Verify Docker daemon is running
- Check container logs with: `docker logs documentdb-gateway-test`

### Data not initialized
- Verify init data files exist in `test-init-data/` directory
- Check file permissions and syntax
- Review container logs for initialization errors

### Connection issues
- Ensure mongosh is installed on the host system
- Verify port 10260 is accessible from localhost
- Ensure mongosh is available in PATH
- Verify authentication credentials
- Check TLS certificate configuration

## Manual Testing

If you want to manually test before the container is stopped, you can run the test and quickly connect in another terminal:

```bash
# In terminal 1: Run the test
./test_init_data.sh

# In terminal 2 (while test is running): Connect manually
mongosh localhost:10260 \
  -u default_user -p TestPassword123 \
  --authenticationMechanism SCRAM-SHA-256 \
  --tls --tlsAllowInvalidCertificates

# Example queries
use('test')
db.users.find({age: {$gt: 30}})
db.products.find({inStock: true})
db.orders.find({status: "completed"})
```

## Container Management

The test script automatically:
- **Builds** the Docker image if needed
- **Starts** the container with initialization data
- **Tests** all functionality
- **Stops** the container when testing is complete

If you need to restart the container for manual testing:

```bash
# Restart the stopped container
docker start documentdb-gateway-test

# Connect manually
mongosh localhost:10260 \
  -u default_user -p TestPassword123 \
  --authenticationMechanism SCRAM-SHA-256 \
  --tls --tlsAllowInvalidCertificates
```

## Cleanup

The test script automatically stops the container when testing is complete. For full cleanup:

```bash
# Remove the test container (if you want to clean up completely)
docker rm documentdb-gateway-test

# Optionally remove the test image
docker rmi documentdb-gateway-test
```
