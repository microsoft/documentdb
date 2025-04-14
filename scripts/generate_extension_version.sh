#!/bin/bash

# fail if trying to reference a variable that is not set.
set -u
set -e

outputFile=$1
extensionName=${2:-""}
extensionDir=${3:-$extensionName}
gitIndexDir=${4:-"unknown"}

source="${BASH_SOURCE[0]}"
while [[ -h $source ]]; do
   scriptroot="$( cd -P "$( dirname "$source" )" && pwd )"
   source="$(readlink "$source")"

   # if $source was a relative symlink, we need to resolve it relative to the path where the
   # symlink file was located
   [[ $source != /* ]] && source="$scriptroot/$source"
done

repoScriptDir="$( cd -P "$( dirname "$source" )" && pwd )"

# Check if inside a Git repository
if [ -f $gitIndexDir ]; then  
    echo "Find index file: $gitIndexDir"  

    if [ ${BUILD_SOURCEBRANCH:-""} != "" ]; then
        # devops branches are of the form refs/heads/foo
        echo "Using DevopsBranch ${BUILD_SOURCEBRANCH} and version ${BUILD_SOURCEVERSION}"
        GIT_VERSION=$(echo ${BUILD_SOURCEBRANCH} | sed -E "s/refs\/heads\/(.+)/\1/i" | sed -E "s/refs\/(.+)\/merge/\1/i" | sed -E "s/\//_/")
        GIT_SHA=${BUILD_SOURCEVERSION:0:7}
    else
        # set GIT_VERSION with current branch's name and the short sha of the HEAD
        GIT_VERSION=$(git rev-parse --abbrev-ref HEAD)
        GIT_SHA=$(git rev-parse --short HEAD)
    fi

    if [[ "$GIT_VERSION" == "" ]] || [[ "$GIT_SHA" == "" ]]; then
        echo "Unable to get git version. Bailing"
        exit 1
    fi

    BUILD_VER=" buildId:${BUILD_BUILDID:-"0"}"

    GIT_SHA=" sha:${GIT_SHA}"
else 
    # In case we are not in a git repo, we will use the GIT_VERSION, BUILD_VER and GIT_SHA as "unknown"
    echo "Not inside a Git repository or Git index file not found."
    
    GIT_VERSION="unknown"

    BUILD_VER=" buildId:unknown"

    GIT_SHA=" sha:unknown"
fi   

controlFile="$extensionName.control"

versionStringOutput=$(sed -n "s/^default_version = '\(.*\)'$/\1/p" $controlFile)
EXTENSION_VERSION_STR=${versionStringOutput/-/.}

echo "Using GitVersion '$GIT_VERSION' with SHA '$GIT_SHA' and $extensionName Version '$EXTENSION_VERSION_STR' and Build '$BUILD_VER'"

# Write the header file (Create a new one in the first line and then append to it)
# An example of what's generated

###############################################
# #define EXTENSION_VERSION_STR "1.0.0"
# #define GIT_VERSION "main sha:123defa"
#########################################

echo "// Autogenerated header for versioning" > $outputFile
echo "#define EXTENSION_VERSION_STR \"${EXTENSION_VERSION_STR}\"" >> $outputFile
echo "#define GIT_VERSION \"${GIT_VERSION}${GIT_SHA}${BUILD_VER}\"" >> $outputFile