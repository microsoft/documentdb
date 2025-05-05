#!/bin/bash

# exit immediately if a command exits with a non-zero status
set -e
# fail if trying to reference a variable that is not set.
set -u

configFile=""
help="false"
clean="false"
createUser="true"
userName=""
userPassword=""
hostname="localhost"
port="9712" # Default port
owner=$(whoami)

while getopts "d:u:p:n:chsP:o:" opt; do
    case $opt in
    d)
        configFile="$OPTARG"
        ;;
    u)
        userName="$OPTARG"
        ;;
    p)
        userPassword="$OPTARG"
        ;;
    n)
        hostname="$OPTARG"
        ;;
    P)
        port="$OPTARG"
        ;;
    o)
        owner="$OPTARG"
        ;;
    c)
        clean="true"
        ;;
    h)
        help="true"
        ;;
    s) createUser="false" ;;
    esac

    # Assume empty string if it's unset since we cannot reference to
    # an unset variable due to "set -u".
    case ${OPTARG:-""} in
    -*)
        echo "Option $opt needs a valid argument. use -h to get help."
        exit 1
        ;;
    esac
done

green=$(tput setaf 2)
if [ "$help" == "true" ]; then
    echo "${green}sets up and launches the documentdb gateway on the port specified in the config."
    echo "${green}build_and_start_gateway.sh [-u <userName>] [-p <userPassword>] [-d <SetupConfigurationFile>] [-n <hostname>] [-s] [-c] [-P <port>] [-o <owner>]"
    echo "${green}[-u] - required argument. username for the user to be created."
    echo "${green}[-p] - required argument. password for the user to be created."
    echo "${green}[-n] - optional argument. hostname for the database connection. Default is localhost."
    echo "${green}[-P] - optional argument. port for the database connection. Default is 9712."
    echo "${green}[-c] - optional argument. runs cargo clean before building the gateway."
    echo "${green}[-d] - optional argument. path to custom SetupConfiguration file"
    echo "${green}[-s] - optional argument. Skips user creation. If provided, -u and -p."
    echo "${green}       are no longer required."
    echo "${green}[-o] - optional argument. specifies the owner for the database operations. Default is postgres."
    echo "${green}if SetupConfigurationFile not specified assumed to be"
    echo "${green}oss/pg_documentdb_gw/SetupConfiguration.json and the default port is 10260"
    exit 1
fi

# Get the script directory
source="${BASH_SOURCE[0]}"
while [[ -L $source ]]; do
    scriptroot="$(cd -P "$(dirname "$source")" && pwd)"
    source="$(readlink "$source")"

    # if $source was a relative symlink, we need to resolve it relative to the path where the
    # symlink file was located
    [[ $source != /* ]] && source="$scriptroot/$source"
done
scriptDir="$(cd -P "$(dirname "$source")" && pwd)"

# Check if PostgreSQL is running with a timeout of 10 minutes
timeout=600
interval=5
elapsed=0

echo "Waiting for PostgreSQL to be ready on $hostname:$port..."
while ! pg_isready -h "$hostname" -p "$port" > /dev/null 2>&1; do
    if [ "$elapsed" -ge "$timeout" ]; then
        echo "PostgreSQL did not become ready within 10 minutes. Exiting."
        exit 1
    fi
    sleep "$interval"
    elapsed=$((elapsed + interval))
done
echo "PostgreSQL is ready."

if [ "$clean" = "true" ]; then
    echo "Cleaning the build directory..."
    cargo clean
fi

if [ "$createUser" = "true" ]; then
    if [ -z "$userName" ]; then
        echo "User name is required. Use -u <userName> to specify the user name."
        exit 1
    fi
    if [ -z "$userPassword" ]; then
        echo "User password is required. Use -p <userPassword> to specify the user password."
        exit 1
    fi

    echo "Setting up user $userName with owner $owner"
    echo "Checking if role $userName exists..."
    if ! psql -h "$hostname" -p "$port" -U "$owner" -d postgres -c "SELECT 1 FROM pg_roles WHERE rolname = '$userName';" | grep -q 1; then
        echo "Role $userName does not exist. Creating role..."
        if ! psql -h "$hostname" -p "$port" -U "$owner" -d postgres -c "CREATE ROLE \"$userName\" WITH LOGIN INHERIT PASSWORD '$userPassword' IN ROLE documentdb_admin_role;"; then
            echo "Failed to create role $userName."
            exit 1
        fi
    else
        echo "Role $userName already exists."
    fi
    if ! psql -h "$hostname" -p "$port" -U "$owner" -d postgres -c "ALTER ROLE \"$userName\" CREATEROLE"; then
        echo "Failed to alter role $userName."
        exit 1
    fi
    if ! psql -h "$hostname" -p "$port" -U "$owner" -d postgres -c "GRANT \"$userName\" TO $owner WITH ADMIN OPTION"; then
        echo "Failed to grant role $userName to $owner."
        exit 1
    fi
else
    echo "Skipping user creation."
fi

if [ -z "$configFile" ]; then
    /home/documentdb/gateway/documentdb_gateway
else
    /home/documentdb/gateway/documentdb_gateway "$configFile"
fi