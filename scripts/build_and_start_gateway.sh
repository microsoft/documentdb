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

while getopts "d:u:p:chs" opt; do
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
    echo "${green}build_and_start_gateway.sh [-u <userName>] [-p <userPassword>] [-d <SetupConfigurationFile>] [-s] [-c]"
    echo "${green}[-u] - required argument. username for the user to be created."
    echo "${green}[-p] - required argument. password for the user to be created."
    echo "${green}[-c] - optional argument. runs cargo clean before building the gateway."
    echo "${green}[-d] - optional argument. path to custom SetupConfiguration file"
    echo "${green}[-s] - optional argument. Skips user creation. If provided, -u and p."
    echo "${green}       are no longer required."
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

pushd $scriptDir/../pg_documentdb_gw

if [ $clean = "true" ]; then
    echo "Cleaning the build directory..."
    cargo clean
fi


if [ $createUser = "true" ]; then
    if [ -z "$userName" ]; then
        echo "User name is required. Use -u <userName> to specify the user name."
        exit 1
    fi
    if [ -z "$userPassword" ]; then
        echo "User password is required. Use -p <userPassword> to specify the user password."
        exit 1
    fi
    port="9712"
    owner=$(whoami)

    echo "Setting up user $userName"
    psql -p $port -U $owner -d postgres -c "CREATE ROLE \"$userName\" WITH LOGIN INHERIT PASSWORD '$userPassword' IN ROLE documentdb_admin_role"
    psql -p $port -U $owner -d postgres -c "ALTER ROLE \"$userName\" CREATEROLE"
    psql -p $port -U $owner -d postgres -c "GRANT \"$userName\" TO $owner WITH ADMIN OPTION"
fi

if [ -z "$configFile" ]; then
    cargo run
else
    cargo run $configFile
fi
