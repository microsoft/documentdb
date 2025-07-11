#!/bin/bash

# Cleanup function to handle container shutdown gracefully
cleanup() {
    echo "Shutting down DocumentDB components..."
    
    # Kill log streaming processes if they exist
    for pid_var in PG_LOG_TAIL_PID SYSTEM_PG_LOG_TAIL_PID OSS_LOG_TAIL_PID GATEWAY_LOG_TAIL_PID; do
        pid_value=$(eval echo \$${pid_var})
        if [ -n "$pid_value" ]; then
            echo "Stopping log streaming process $pid_var (PID: $pid_value)"
            kill $pid_value 2>/dev/null || true
        fi
    done
    
    # Kill gateway process if it exists
    if [ -n "${gateway_pid:-}" ]; then
        echo "Stopping gateway process (PID: $gateway_pid)"
        kill $gateway_pid 2>/dev/null || true
    fi
    
    echo "Cleanup completed"
    exit 0
}

# Set up signal handlers for graceful shutdown
trap cleanup SIGTERM SIGINT

# Function to start log streaming
start_log_streaming() {
    local log_file="$1"
    local log_prefix="$2"
    local pid_var="$3"
    
    if [ -f "$log_file" ]; then
        echo "Starting log streaming from $log_file with prefix [$log_prefix]..."
        tail -F "$log_file" 2>/dev/null | while IFS= read -r line; do
            echo "[$log_prefix] $line"
        done &
        local tail_pid=$!
        eval "$pid_var=$tail_pid"
        echo "Log streaming started with PID: $tail_pid"
        return 0
    else
        echo "Log file $log_file not found, skipping streaming"
        return 1
    fi
}

# Print help message
usage() {
    cat << EOF
Launches DocumentDB

Optional arguments:
  -h, --help            Display information on available configuration
  --enforce-ssl         Enforce SSL for all connections. 
                        Defaults to true
                        Overrides ENFORCE_SSL environment variable.
  --cert-path [PATH]    Specify a path to a certificate for securing traffic. You need to mount this file into the 
                        container (e.g. if CERT_PATH=/mycert.pfx, you'd add an option like the following to your 
                        docker run: --mount type=bind,source=./mycert.pfx,target=/mycert.pfx)
                        Can set CERT_SECRET to the password for the certificate.
                        Overrides CERT_PATH environment variable.
  --key-file [PATH]     Override default key with key in key file. You need to mount this file into the 
                        container (e.g. if KEY_FILE=/mykey.key, you'd add an option like the following to your 
                        docker run: --mount type=bind,source=./mykey.key,target=/mykey.key)
                        Overrides KEY_FILE environment variable.
  --data-path [PATH]    Specify a directory for data. Frequently used with docker run --mount option 
                        (e.g. if DATA_PATH=/usr/documentdb/data, you'd add an option like the following to your 
                        docker run: --mount type=bind,source=./.local/data,target=/usr/documentdb/data)
                        Defaults to /data
                        Overrides DATA_PATH environment variable.
  --documentdb-port     The port of the DocumentDB endpoint on the container. 
                        You still need to publish this port (e.g. -p 10260:10260).
                        Defaults to 10260
                        Overrides PORT environment variable.
  --enable-telemetry    Enable telemetry data sent to the usage colletor (Azure Application Insights). 
                        Overrides ENABLE_TELEMETRY environment variable.
  --log-level           The verbosity of logs that will be emitted.
                        Overrides LOG_LEVEL environment variable.
                          quiet, error, warn, info (default), debug, trace
  --username            Specify the username for the DocumentDB.
                        Defaults to default_user
                        Overrides USERNAME environment variable.
  --password            Specify the password for the DocumentDB.
                        REQUIRED.
                        Overrides PASSWORD environment variable.
  --create-user         Specify whether to create a user. 
                        Defaults to true.
  --start-pg            Specify whether to start the PostgreSQL server.
                        Defaults to true.
  --pg-port             Specify the port for the PostgreSQL server.
                        Defaults to 9712.
                        Overrides PG_PORT environment variable.
  --owner               Specify the owner of the DocumentDB.
                        Overrides OWNER environment variable.
                        defaults to documentdb.
  --allow-external-connections
                        Allow external connections to PostgreSQL.
                        Defaults to false.
                        Overrides ALLOW_EXTERNAL_CONNECTIONS environment variable.
                        
EOF
}

if [[ -f "/version.txt" ]]; then
  DocumentDB_RELEASE_VERSION=$(cat /version.txt)
  echo "Release Version: $DocumentDB_RELEASE_VERSION"
fi

echo "[ENTRYPOINT] DocumentDB container starting..."
echo "[ENTRYPOINT] Log streaming will be enabled for PostgreSQL logs"

# Handle arguments

while [[ $# -gt 0 ]];
do
  case $1 in
    -h|--help) 
        usage;
        exit 0;;

    --enforce-ssl)
        shift
        export ENFORCE_SSL=$1
        shift;;

    --cert-path)
        shift
        export CERT_PATH=$1
        shift;;

    --key-file)
        shift
        export KEY_FILE=$1
        shift;;

    --data-path)
        shift
        export DATA_PATH=$1
        shift;;

    --documentdb-port)
        shift
        export DOCUMENTDB_PORT=$1
        shift;;

    --enable-telemetry)
        shift
        export ENABLE_TELEMETRY=$1
        shift;;
        
    --log-level)
        shift
        export LOG_LEVEL=$1
        shift;;

    --username)
        shift
        export USERNAME=$1
        shift;;

    --password)
        shift
        export PASSWORD=$1
        shift;;

    --create-user)
        shift
        export CREATE_USER=$1
        shift;;

    --start-pg)
        shift
        export START_POSTGRESQL=$1
        shift;;

    --pg-port)
        shift
        export POSTGRESQL_PORT=$1
        shift;;

    --owner)
        shift
        export OWNER=$1
        shift;;

    --allow-external-connections)
        shift
        export ALLOW_EXTERNAL_CONNECTIONS=$1
        shift;;

    -*)
        echo "Unknown option $1"
        exit 1;; 
  esac
done

# Set default values if not provided
export OWNER=${OWNER:-$(whoami)}
export DATA_PATH=${DATA_PATH:-/data}
export DOCUMENTDB_PORT=${DOCUMENTDB_PORT:-10260}
export POSTGRESQL_PORT=${POSTGRESQL_PORT:-9712}
export USERNAME=${USERNAME:-default_user}
export PASSWORD=${PASSWORD:-Admin100}
export CREATE_USER=${CREATE_USER:-true}
export START_POSTGRESQL=${START_POSTGRESQL:-true}

# Setup centralized log directory structure
echo "Setting up centralized log directory at /var/log/documentdb..."
sudo mkdir -p /var/log/documentdb/postgres
sudo chown -R documentdb:documentdb /var/log/documentdb
sudo chmod -R 755 /var/log/documentdb

# Define centralized log file paths
export ENTRYPOINT_LOG="/var/log/documentdb/gateway_entrypoint.log"
export GATEWAY_LOG="/var/log/documentdb/gateway.log"
export OSS_SERVER_LOG="/var/log/documentdb/oss_server.log"
# Note: PostgreSQL log will be symlinked after PostgreSQL starts
export PG_LOG_FILE="/var/log/documentdb/postgres/pglog.log"

echo "Centralized log directory created with the following structure:"
echo "  /var/log/documentdb/gateway_entrypoint.log"
echo "  /var/log/documentdb/gateway.log"
echo "  /var/log/documentdb/oss_server.log"
echo "  /var/log/documentdb/postgres/pglog.log (will be symlinked)"

# Validate required parameters
if [ -z "${PASSWORD:-}" ]; then
    echo "Error: PASSWORD is required. Please provide a password using --password argument or PASSWORD environment variable."
    exit 1
fi

echo "Using username: $USERNAME"
echo "Using owner: $OWNER"
echo "Using data path: $DATA_PATH"

if { [ -n "${CERT_PATH:-}" ] && [ -z "${KEY_FILE:-}" ]; } || \
   { [ -z "${CERT_PATH:-}" ] && [ -n "${KEY_FILE:-}" ]; }; then
    echo "Error: Both CERT_PATH and KEY_FILE must be set together, or neither should be set."
    exit 1
fi

if { [ -z "${CERT_PATH:-}" ] && [ -z "${KEY_FILE:-}" ]; }; then
    echo "CERT_PATH and KEY_FILE not provided. Generating self-signed certificate and key..."
    CERT_PATH="$HOME/self_signed_cert.pem"
    KEY_FILE="$HOME/self_signed_key.pem"
    openssl req -x509 -newkey rsa:4096 -keyout "$KEY_FILE" -out "$CERT_PATH" -days 365 -nodes -subj "/CN=localhost"
    
    echo "Generated certificate at $CERT_PATH and key at $KEY_FILE."

    # Combine the key and certificate into a single PEM file
    COMBINED_PEM="$HOME/self_signed_combined.pem"
    cat "$CERT_PATH" "$KEY_FILE" > "$COMBINED_PEM"
    echo "Combined certificate and key into $COMBINED_PEM."

    # Export docker cp command for copying the PEM file to the local machine
    echo "To copy the combined PEM file to your local machine, use the following command:"
    echo "docker cp <container_id>:$COMBINED_PEM ./self_signed_combined.pem"
fi

num='^[0-9]+$'
if ! [[ "$DOCUMENTDB_PORT" =~ $num ]]; then
    echo "Invalid port value $DOCUMENTDB_PORT, must be a number"
    exit 1
fi

if ! [[ "$POSTGRESQL_PORT" =~ $num ]]; then
    echo "Invalid PostgreSQL port value $POSTGRESQL_PORT, must be a number"
    exit 1
fi

if [ -n "$ENABLE_TELEMETRY" ] && \
   [ "$ENABLE_TELEMETRY" != "true" ] && \
   [ "$ENABLE_TELEMETRY" != "false" ]; then
    echo "Invalid enable-telemetry value $ENABLE_TELEMETRY, must be true or false"
    exit 1
fi

if [ -n "$LOG_LEVEL" ] && \
   [ "$LOG_LEVEL" != "quiet" ] && \
   [ "$LOG_LEVEL" != "error" ] && \
   [ "$LOG_LEVEL" != "warn" ] && \
   [ "$LOG_LEVEL" != "info" ] && \
   [ "$LOG_LEVEL" != "debug" ] && \
   [ "$LOG_LEVEL" != "trace" ]; then
    echo "Invalid log level value $LOG_LEVEL, must be one of: quiet, error, warn, info, debug, trace"
    exit 1
fi

if [ "$START_POSTGRESQL" = "true" ]; then
    echo "Starting PostgreSQL server on port $POSTGRESQL_PORT..."
    exec > >(tee -a "$ENTRYPOINT_LOG") 2> >(tee -a "$ENTRYPOINT_LOG" >&2)
    
    # Fix permissions on data directory to prevent "Permission denied" errors
    echo "Ensuring proper permissions on data directory: $DATA_PATH"
    if [ ! -d "$DATA_PATH" ]; then
        echo "Creating data directory: $DATA_PATH"
        sudo mkdir -p "$DATA_PATH"
    fi
    
    # Change ownership to documentdb user to ensure we can write/delete files
    echo "Setting ownership of $DATA_PATH to documentdb user"
    sudo chown -R documentdb:documentdb "$DATA_PATH"
    
    # Ensure we have full permissions on the directory
    echo "Setting permissions on $DATA_PATH"
    sudo chmod -R 750 "$DATA_PATH"
    
    if ALLOW_EXTERNAL_CONNECTIONS="true"; then
        echo "Allowing external connections to PostgreSQL..."
        export PGOPTIONS="-e"
    fi
    echo "Starting OSS server..."
    /home/documentdb/gateway/scripts/start_oss_server.sh $PGOPTIONS -d $DATA_PATH -p $POSTGRESQL_PORT | tee -a "$OSS_SERVER_LOG"

    echo "OSS server started."
    echo "[ENTRYPOINT] Setting up PostgreSQL log streaming..."

    # Start streaming PostgreSQL logs to docker logs
    # Note: We exclude ENTRYPOINT_LOG_TAIL_PID to prevent recursion
    PG_LOG_TAIL_PID=""
    SYSTEM_PG_LOG_TAIL_PID=""
    OSS_LOG_TAIL_PID=""
    GATEWAY_LOG_TAIL_PID=""
    
    echo "Setting up PostgreSQL log streaming from $PG_LOG_FILE..."
    
    # Wait for PostgreSQL log file to be created in the data directory
    ACTUAL_PG_LOG="$DATA_PATH/pglog.log"
    i=0
    while [ ! -f "$ACTUAL_PG_LOG" ] && [ $i -lt 30 ]; do
        sleep 1
        i=$((i + 1))
    done
    
    # Create symlink from centralized location to actual PostgreSQL log
    if [ -f "$ACTUAL_PG_LOG" ]; then
        echo "Creating symlink: $PG_LOG_FILE -> $ACTUAL_PG_LOG"
        ln -sf "$ACTUAL_PG_LOG" "$PG_LOG_FILE"
    else
        echo "Warning: PostgreSQL log file not found at $ACTUAL_PG_LOG"
    fi
    
    # Start streaming main PostgreSQL log
    start_log_streaming "$PG_LOG_FILE" "POSTGRES" "PG_LOG_TAIL_PID"
    
    # Also stream system PostgreSQL logs if they exist
    SYSTEM_PG_LOG="/var/log/postgresql/postgresql-17-main.log"
    start_log_streaming "$SYSTEM_PG_LOG" "POSTGRES-SYSTEM" "SYSTEM_PG_LOG_TAIL_PID"
    
    # Stream OSS server logs
    start_log_streaming "$OSS_SERVER_LOG" "OSS-SERVER" "OSS_LOG_TAIL_PID"
    
    # NOTE: We do NOT stream entrypoint logs to prevent infinite recursion!
    # The entrypoint messages are already going to stdout/stderr and appear in docker logs
    # ENTRYPOINT_LOG="/home/documentdb/gateway_entrypoint.log"
    # start_log_streaming "$ENTRYPOINT_LOG" "ENTRYPOINT" "ENTRYPOINT_LOG_TAIL_PID"

    echo "Checking if PostgreSQL is running..."
    i=0
    while [ ! -f "$DATA_PATH/postmaster.pid" ]; do
        sleep 1
        if [ $i -ge 60 ]; then
            echo "PostgreSQL failed to start within 60 seconds."
            cat "$OSS_SERVER_LOG"
            exit 1
        fi
        i=$((i + 1))
    done
    echo "PostgreSQL is running."
else
    echo "Skipping PostgreSQL server start."
fi

# Setting up the configuration file
cp /home/documentdb/gateway/SetupConfiguration.json /home/documentdb/gateway/SetupConfiguration_temp.json

if [ -n "${DOCUMENTDB_PORT:-}" ]; then
    echo "Updating GatewayListenPort in the configuration file..."
    jq ".GatewayListenPort = $DOCUMENTDB_PORT" /home/documentdb/gateway/SetupConfiguration_temp.json > /home/documentdb/gateway/SetupConfiguration_temp.json.tmp && \
    mv /home/documentdb/gateway/SetupConfiguration_temp.json.tmp /home/documentdb/gateway/SetupConfiguration_temp.json
fi

if [ -n "${POSTGRESQL_PORT:-}" ]; then
    echo "Updating PostgresPort in the configuration file..."
    jq ".PostgresPort = $POSTGRESQL_PORT" /home/documentdb/gateway/SetupConfiguration_temp.json > /home/documentdb/gateway/SetupConfiguration_temp.json.tmp && \
    mv /home/documentdb/gateway/SetupConfiguration_temp.json.tmp /home/documentdb/gateway/SetupConfiguration_temp.json
fi

if [ -n "${CERT_PATH:-}" ] && [ -n "${KEY_FILE:-}" ]; then
    echo "Adding CertificateOptions to the configuration file..."
    jq --arg certPath "$CERT_PATH" --arg keyFilePath "$KEY_FILE" \
       '. + { "CertificateOptions": { "CertType": "PemFile", "FilePath": $certPath, "KeyFilePath": $keyFilePath } }' \
       /home/documentdb/gateway/SetupConfiguration_temp.json > /home/documentdb/gateway/SetupConfiguration_temp.json.tmp && \
    mv /home/documentdb/gateway/SetupConfiguration_temp.json.tmp /home/documentdb/gateway/SetupConfiguration_temp.json
fi

if [ -n "${ENFORCE_SSL:-}" ]; then
    echo "Updating EnforceSslTcp in the configuration file..."
    jq --arg enforceSsl $ENFORCE_SSL \
       '.EnforceSslTcp = ($enforceSsl == "true")' /home/documentdb/gateway/SetupConfiguration_temp.json > /home/documentdb/gateway/SetupConfiguration_temp.json.tmp && \
    mv /home/documentdb/gateway/SetupConfiguration_temp.json.tmp /home/documentdb/gateway/SetupConfiguration_temp.json
fi

sudo chmod 755 /home/documentdb/gateway/SetupConfiguration_temp.json

configFile="/home/documentdb/gateway/SetupConfiguration_temp.json"

echo "Starting gateway in the background..."
if [ "$CREATE_USER" = "false" ]; then
    echo "Skipping user creation and starting the gateway..."
    /home/documentdb/gateway/scripts/build_and_start_gateway.sh -s -d $configFile -P $POSTGRESQL_PORT -o $OWNER | tee -a "$GATEWAY_LOG" &
else
    /home/documentdb/gateway/scripts/build_and_start_gateway.sh -u $USERNAME -p $PASSWORD -d $configFile -P $POSTGRESQL_PORT -o $OWNER | tee -a "$GATEWAY_LOG" &
fi

gateway_pid=$! # Capture the PID of the gateway process

# Also stream existing gateway logs (for historical logs that might already exist)
if [ -f "$GATEWAY_LOG" ]; then
    echo "Starting gateway log streaming for existing logs..."
    tail -F "$GATEWAY_LOG" 2>/dev/null | while IFS= read -r line; do
        echo "[GATEWAY-FILE] $line"
    done &
    GATEWAY_LOG_TAIL_PID=$!
    echo "Gateway log file streaming started with PID: $GATEWAY_LOG_TAIL_PID"
fi

echo "Gateway started with PID: $gateway_pid"
echo ""
echo "=== DocumentDB is ready ==="
echo "All logs are being streamed to docker logs with prefixes:"
echo "  [POSTGRES] - PostgreSQL database logs ($PG_LOG_FILE)"
echo "  [POSTGRES-SYSTEM] - System PostgreSQL logs (/var/log/postgresql/postgresql-17-main.log)"
echo "  [OSS-SERVER] - OSS server logs ($OSS_SERVER_LOG)"
echo "  [ENTRYPOINT] - Entrypoint script logs ($ENTRYPOINT_LOG)"
echo "  [GATEWAY] - Gateway application logs (live output via tee)"
echo "  [GATEWAY-FILE] - Gateway log file content ($GATEWAY_LOG)"
echo ""
echo "Centralized log directory structure:"
echo "  /var/log/documentdb/"
echo "  ├── gateway_entrypoint.log"
echo "  ├── gateway.log"
echo "  ├── oss_server.log"
echo "  └── postgres/"
echo "      └── pglog.log -> $DATA_PATH/pglog.log"
echo ""
echo "View all logs with: docker logs <container_name>"
echo "View live logs with: docker logs -f <container_name>"
echo "Filter specific logs: docker logs <container_name> | grep '[PREFIX]'"
echo "Example: docker logs <container_name> | grep '[POSTGRES]'"
echo "=========================="
echo ""

# Wait for the gateway process to keep the container alive
# The wait will be interrupted by signals, allowing cleanup to run
wait $gateway_pid