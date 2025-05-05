#!/bin/bash

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
                        Defaults to /home/documentdb/postgresql/data
                        Overrides DATA_PATH environment variable.
  --documentdb-port     The port of the DocumentDB endpoint on the container. 
                        You still need to publish this port (e.g. -p 10260:10260).
                        Defaults to 10260
                        Overrides PORT environment variable.
  --enable-telemetry    Enable telemetry data sent to the usage colletor (Azure Application Insights). 
                        Overrides ENABLE_TELEMETRY environment variable.
  --log-level           The verbosity of logs that will be emitted by the emulator and data explorer.
                        Overrides LOG_LEVEL environment variable.
                          quiet, error, warn, info (default), debug, trace
  --username            Specify the username for the DocumentDB emulator.
                        Defaults to default_user
                        Overrides USERNAME environment variable.
  --password            Specify the password for the DocumentDB emulator.
                        Defaults to Admin100
                        Overrides PASSWORD environment variable.
  --create-user         Specify whether to create a user. 
                        Defaults to true.
  --start-pg            Specify whether to start the PostgreSQL server.
                        Defaults to true.
  --pg-port             Specify the port for the PostgreSQL server.
                        Defaults to 9712.
                        Overrides PG_PORT environment variable.
  --owner               Specify the owner of the DocumentDB emulator.
                        Overrides OWNER environment variable.
                        defaults to documentdb.
  --allow-external-connections
                        Allow external connections to PostgreSQL.
                        Defaults to false.
                        Overrides ALLOW_EXTERNAL_CONNECTIONS environment variable.
                        
EOF
}

if [[ -f "/version.txt" ]]; then
  DocumentDB_EMULATOR_RELEASE_VERSION=$(cat /version.txt)
  echo "Release Version: $DocumentDB_EMULATOR_RELEASE_VERSION"
fi

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
echo "Using username: $USERNAME"
echo "Using owner: $OWNER"

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
    exec > >(tee -a /home/documentdb/gateway_entrypoint.log) 2> >(tee -a /home/documentdb/gateway_entrypoint.log >&2)
    if ALLOW_EXTERNAL_CONNECTIONS="true"; then
        echo "Allowing external connections to PostgreSQL..."
        export PGOPTIONS="-e"
    fi
    echo "Starting OSS server..."
    /home/documentdb/gateway/scripts/start_oss_server.sh $PGOPTIONS -c -d $DATA_PATH -p $POSTGRESQL_PORT | tee -a /home/documentdb/oss_server.log

    echo "OSS server started."

    echo "Checking if PostgreSQL is running..."
    i=0
    while [ ! -f "$DATA_PATH/postmaster.pid" ]; do
        sleep 1
        if [ $i -ge 60 ]; then
            echo "PostgreSQL failed to start within 60 seconds."
            cat /home/documentdb/oss_server.log
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
    echo "Updating MongoListenPort in the configuration file..."
    jq ".MongoListenPort = $DOCUMENTDB_PORT" /home/documentdb/gateway/SetupConfiguration_temp.json > /home/documentdb/gateway/SetupConfiguration_temp.json.tmp && \
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
    /home/documentdb/gateway/scripts/build_and_start_gateway.sh -s -d $configFile -P $POSTGRESQL_PORT -o $OWNER | tee -a /home/documentdb/gateway.log &
else
    /home/documentdb/gateway/scripts/build_and_start_gateway.sh -u $USERNAME -p $PASSWORD -d $configFile -P $POSTGRESQL_PORT -o $OWNER | tee -a /home/documentdb/gateway.log &
fi

gateway_pid=$! # Capture the PID of the gateway process

# Wait for the gateway process to keep the container alive
wait $gateway_pid