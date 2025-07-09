#!/bin/bash

# Script to test the init-data-path feature of DocumentDB
set -e

CONTAINER_NAME="documentdb-gateway-test"
IMAGE_NAME="documentdb-gateway-test"
INIT_DATA_DIR="/home/song/documentdb/test-init-data"
DOCUMENTDB_PORT="10260"
PASSWORD="TestPassword123"

echo "=== DocumentDB Init-Data-Path Feature Test ==="
echo "Container: $CONTAINER_NAME"
echo "Image: $IMAGE_NAME"
echo "Init Data Directory: $INIT_DATA_DIR"
echo "DocumentDB Port: $DOCUMENTDB_PORT"
echo

# Function to cleanup previous runs
cleanup() {
    echo "Cleaning up previous containers..."
    docker stop $CONTAINER_NAME 2>/dev/null || true
    docker rm $CONTAINER_NAME 2>/dev/null || true
}

# Function to wait for DocumentDB to be ready
wait_for_documentdb() {
    echo "Waiting for DocumentDB to be ready..."
    local max_attempts=30
    local attempt=1
    
    while [ $attempt -le $max_attempts ]; do
        if docker exec $CONTAINER_NAME mongosh localhost:$DOCUMENTDB_PORT -u default_user -p $PASSWORD --authenticationMechanism SCRAM-SHA-256 --tls --tlsAllowInvalidCertificates --eval "db.runCommand({ping: 1})" >/dev/null 2>&1; then
            echo "DocumentDB is ready!"
            return 0
        fi
        
        echo "Attempt $attempt/$max_attempts - waiting..."
        sleep 2
        attempt=$((attempt + 1))
    done
    
    echo "Error: DocumentDB did not become ready"
    return 1
}

# Function to verify the initialized data
verify_data() {
    echo "=== Verifying Initialized Data ==="
    
    # Check users collection
    echo "Checking users collection..."
    USER_COUNT=$(docker exec $CONTAINER_NAME mongosh localhost:$DOCUMENTDB_PORT -u default_user -p $PASSWORD --authenticationMechanism SCRAM-SHA-256 --tls --tlsAllowInvalidCertificates --eval "use('test'); db.users.countDocuments()" --quiet)
    echo "Users count: $USER_COUNT"
    
    # Check products collection
    echo "Checking products collection..."
    PRODUCT_COUNT=$(docker exec $CONTAINER_NAME mongosh localhost:$DOCUMENTDB_PORT -u default_user -p $PASSWORD --authenticationMechanism SCRAM-SHA-256 --tls --tlsAllowInvalidCertificates --eval "use('test'); db.products.countDocuments()" --quiet)
    echo "Products count: $PRODUCT_COUNT"
    
    # Check orders collection
    echo "Checking orders collection..."
    ORDER_COUNT=$(docker exec $CONTAINER_NAME mongosh localhost:$DOCUMENTDB_PORT -u default_user -p $PASSWORD --authenticationMechanism SCRAM-SHA-256 --tls --tlsAllowInvalidCertificates --eval "use('test'); db.orders.countDocuments()" --quiet)
    echo "Orders count: $ORDER_COUNT"
    
    # Show sample data from each collection
    echo
    echo "=== Sample Data ==="
    echo "Sample user:"
    docker exec $CONTAINER_NAME mongosh localhost:$DOCUMENTDB_PORT -u default_user -p $PASSWORD --authenticationMechanism SCRAM-SHA-256 --tls --tlsAllowInvalidCertificates --eval "use('test'); db.users.findOne()" --quiet
    
    echo
    echo "Sample product:"
    docker exec $CONTAINER_NAME mongosh localhost:$DOCUMENTDB_PORT -u default_user -p $PASSWORD --authenticationMechanism SCRAM-SHA-256 --tls --tlsAllowInvalidCertificates --eval "use('test'); db.products.findOne()" --quiet
    
    echo
    echo "Sample order:"
    docker exec $CONTAINER_NAME mongosh localhost:$DOCUMENTDB_PORT -u default_user -p $PASSWORD --authenticationMechanism SCRAM-SHA-256 --tls --tlsAllowInvalidCertificates --eval "use('test'); db.orders.findOne()" --quiet
    
    # Verify indexes were created
    echo
    echo "=== Checking Indexes ==="
    echo "Users indexes:"
    docker exec $CONTAINER_NAME mongosh localhost:$DOCUMENTDB_PORT -u default_user -p $PASSWORD --authenticationMechanism SCRAM-SHA-256 --tls --tlsAllowInvalidCertificates --eval "use('test'); db.users.getIndexes()" --quiet
    
    echo
    echo "Products indexes:"
    docker exec $CONTAINER_NAME mongosh localhost:$DOCUMENTDB_PORT -u default_user -p $PASSWORD --authenticationMechanism SCRAM-SHA-256 --tls --tlsAllowInvalidCertificates --eval "use('test'); db.products.getIndexes()" --quiet
    
    echo
    echo "Orders indexes:"
    docker exec $CONTAINER_NAME mongosh localhost:$DOCUMENTDB_PORT -u default_user -p $PASSWORD --authenticationMechanism SCRAM-SHA-256 --tls --tlsAllowInvalidCertificates --eval "use('test'); db.orders.getIndexes()" --quiet
}

# Main test execution
main() {
    # Cleanup any previous test runs
    cleanup
    
    # Check if image exists
    if ! docker images | grep -q $IMAGE_NAME; then
        echo "Error: Docker image $IMAGE_NAME not found!"
        echo "Please build the image first with:"
        echo "docker build -f .github/containers/Build-Ubuntu/Dockerfile_gateway -t $IMAGE_NAME ."
        exit 1
    fi
    
    # Run the container with our test data mounted
    echo "Starting DocumentDB container with init data..."
    docker run -d \
        --name $CONTAINER_NAME \
        -p $DOCUMENTDB_PORT:$DOCUMENTDB_PORT \
        -e PASSWORD=$PASSWORD \
        -v "$INIT_DATA_DIR:/docker-entrypoint-initdb.d" \
        $IMAGE_NAME \
        --password $PASSWORD \
        --init-data-path /docker-entrypoint-initdb.d
    
    # Wait for the container to be ready
    if wait_for_documentdb; then
        # Give a bit more time for initialization to complete
        echo "Waiting for data initialization to complete..."
        sleep 10
        
        # Verify the data was loaded
        verify_data
        
        echo
        echo "=== Test Summary ==="
        if [ "$USER_COUNT" = "4" ] && [ "$PRODUCT_COUNT" = "4" ] && [ "$ORDER_COUNT" = "4" ]; then
            echo "✅ SUCCESS: All test data was successfully initialized!"
            echo "   - Users: $USER_COUNT/4"
            echo "   - Products: $PRODUCT_COUNT/4" 
            echo "   - Orders: $ORDER_COUNT/4"
        else
            echo "❌ FAILURE: Data initialization incomplete!"
            echo "   - Users: $USER_COUNT/4 expected"
            echo "   - Products: $PRODUCT_COUNT/4 expected"
            echo "   - Orders: $ORDER_COUNT/4 expected"
            
            # Show container logs for debugging
            echo
            echo "=== Container Logs ==="
            docker logs $CONTAINER_NAME
        fi
    else
        echo "❌ FAILURE: DocumentDB failed to start properly"
        echo
        echo "=== Container Logs ==="
        docker logs $CONTAINER_NAME
    fi
    
    echo
    echo "Container is still running. You can:"
    echo "1. Connect manually: docker exec -it $CONTAINER_NAME mongosh localhost:$DOCUMENTDB_PORT -u default_user -p $PASSWORD --authenticationMechanism SCRAM-SHA-256 --tls --tlsAllowInvalidCertificates"
    echo "2. Stop container: docker stop $CONTAINER_NAME"
    echo "3. Remove container: docker rm $CONTAINER_NAME"
}

# Run the test
main "$@"
