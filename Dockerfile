ARG PG_VERSION=16

FROM --platform=linux/amd64 mcr.microsoft.com/mirror/docker/library/ubuntu:20.04 AS build-image
ARG POSTGRES_INSTALL_ARG=
ARG PG_VERSION=16
ARG CITUS_VERSION="v12.1"

# declare installed PG version and Citus version
ENV PG_VERSION=${PG_VERSION}
ENV CITUS_VERSION=${CITUS_VERSION}

# Install build essentials - Compiler, debugger, make, etc.
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -qy \
    wget \
    curl \
    sudo \
    gnupg2 \
    lsb-release \
    tzdata \
    build-essential \
    pkg-config \
    cmake \
    git \
	locales \
	gcc \
    gdb \
    libipc-run-perl \
    unzip \
    apt-transport-https \
    bison \
    flex \
    libreadline-dev \
    zlib1g-dev \
    libkrb5-dev \
    software-properties-common \
    libtool \
    libicu-dev \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Add pgdg repo
RUN echo "deb http://apt.postgresql.org/pub/repos/apt/ $(lsb_release -cs)-pgdg main" | tee /etc/apt/sources.list.d/pgdg.list && \
    wget --quiet -O - https://www.postgresql.org/media/keys/ACCC4CF8.asc | apt-key add - && \
    apt-get update && \
    rm -rf /var/lib/apt/lists/*

# Prepare for running install scripts
ENV CLEANUP_SETUP=1
ENV INSTALL_DEPENDENCIES_ROOT=/tmp/install_setup
RUN mkdir -p /tmp/install_setup

# Copy setup_versions.sh which decides versions of the dependencies to install.
COPY scripts/setup_versions.sh /tmp/install_setup/

# Install libbson
COPY scripts/install_setup_libbson.sh /tmp/install_setup
RUN [ "bin/bash", "-c", "export MAKE_PROGRAM=cmake && /tmp/install_setup/install_setup_libbson.sh" ]

# Copy utils.sh
COPY scripts/utils.sh /tmp/install_setup/

# Install postgres
COPY scripts/install_setup_postgres.sh /tmp/install_setup/
RUN [ "bin/bash", "-c", "/tmp/install_setup/install_setup_postgres.sh -d \"/usr/lib/postgresql/${PG_VERSION}\" $POSTGRES_INSTALL_ARG -v ${PG_VERSION}" ]

# Install RUM from source
COPY scripts/install_setup_rum_oss.sh /tmp/install_setup/
RUN [ "bin/bash", "-c", "PGVERSION=$PG_VERSION /tmp/install_setup/install_setup_rum_oss.sh" ]

# Install citus
COPY scripts/install_setup_citus_core_oss.sh /tmp/install_setup/
RUN [ "bin/bash", "-c", "PGVERSION=$PG_VERSION /tmp/install_setup/install_setup_citus_core_oss.sh ${CITUS_VERSION}" ]

# Install citus-indent
COPY scripts/install_citus_indent.sh /tmp/install_setup/
RUN [ "bin/bash", "-c", "/tmp/install_setup/install_citus_indent.sh" ]

# Install SYSTEM_ROWS
COPY scripts/install_setup_system_rows.sh /tmp/install_setup/
RUN [ "bin/bash", "-c", "PGVERSION=$PG_VERSION /tmp/install_setup/install_setup_system_rows.sh" ]

# Install PG_CRON
COPY scripts/install_setup_pg_cron.sh /tmp/install_setup/
RUN [ "bin/bash", "-c", "PGVERSION=$PG_VERSION /tmp/install_setup/install_setup_pg_cron.sh" ]

# Download Decimal128 Intel library
COPY scripts/install_setup_intel_decimal_math_lib.sh /tmp/install_setup/
RUN [ "bin/bash", "-c", "/tmp/install_setup/install_setup_intel_decimal_math_lib.sh" ]

# Download PCRE2 library
COPY scripts/install_setup_pcre2.sh /tmp/install_setup/
RUN [ "bin/bash", "-c", "/tmp/install_setup/install_setup_pcre2.sh" ]

# Install PG_VECTOR
COPY scripts/install_setup_pgvector.sh /tmp/install_setup/
RUN [ "bin/bash", "-c", "PGVERSION=$PG_VERSION /tmp/install_setup/install_setup_pgvector.sh" ]

# Install PostGIS from source
RUN add-apt-repository -y ppa:ubuntugis/ppa && apt-get update && \
    apt-get install -qy \
    libproj-dev \
    libxml2-dev \
    libjson-c-dev \
    libgeos++-dev libgeos-3.9.1 libgeos-c1v5 libgeos-dev \
    && rm -rf /var/lib/apt/lists/*
COPY scripts/install_setup_postgis.sh /tmp/install_setup/
RUN [ "bin/bash", "-c", "PGVERSION=$PG_VERSION /tmp/install_setup/install_setup_postgis.sh" ]

# locale
RUN rm -rf /var/lib/apt/lists/* \
    && localedef -i en_US -c -f UTF-8 -A /usr/share/locale/locale.alias en_US.UTF-8

ENV LANG en_US.UTF-8
ENV LC_ALL en_US.UTF-8
ENV LANGUAGE en_US

# Create documentdb user
RUN useradd -ms /bin/bash documentdb -G sudo
RUN echo "%sudo ALL=(ALL:ALL) NOPASSWD: ALL" >> /etc/sudoers.d/no-pass-ask

# Add postgres to path for sudo commands.
# Add path for sudoers
RUN cat /etc/sudoers | grep secure_path | sed "s/\:\/bin\:/\:\/bin\:\/usr\/lib\/postgresql\/$PG_VERSION\/bin\:/" >> /etc/sudoers.d/postgres_path

# Add PG to the path
ENV PATH=$PATH:/usr/lib/postgresql/$PG_VERSION/bin

COPY / /home/documentdb/code/
RUN git config --global --add safe.directory /home/documentdb/code
WORKDIR /home/documentdb/code
RUN make \
    && sudo make install

WORKDIR /

#================================================
# Final image
#================================================

FROM mcr.microsoft.com/mirror/docker/library/ubuntu:20.04 AS final
ARG PG_VERSION=16
ENV PG_VERSION=${PG_VERSION}
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC

# Install dependencies for adding repositories
RUN apt-get update && apt-get install -y \
    wget \
    gnupg2 \
    lsb-release \
    tzdata

# Preconfigure tzdata to avoid interactive prompts
RUN ln -fs /usr/share/zoneinfo/${TZ} /etc/localtime && \
    dpkg-reconfigure --frontend noninteractive tzdata

# Add the PostgreSQL APT repository and import its key
RUN echo "deb http://apt.postgresql.org/pub/repos/apt/ $(lsb_release -cs)-pgdg main" > /etc/apt/sources.list.d/pgdg.list && \
    wget --quiet -O - https://www.postgresql.org/media/keys/ACCC4CF8.asc | apt-key add -

# Update package list and install PostgreSQL with the specified version
RUN apt-get update && apt-get install -y \
    postgresql-${PG_VERSION}  \
    postgresql-server-dev-${PG_VERSION} \
    sudo vim

COPY --from=build-image /usr/lib/postgresql/${PG_VERSION}/lib/ /tmp/postgres-lib/
RUN (mkdir -p /usr/lib/postgresql/${PG_VERSION}/lib/ || true) && \
    cp -rn /tmp/postgres-lib/* /usr/lib/postgresql/${PG_VERSION}/lib/ && \
    rm -rf /tmp/postgres-lib

COPY --from=build-image /usr/lib/postgresql/${PG_VERSION}/include/ /tmp/postgres-include/
RUN (mkdir -p /usr/lib/postgresql/${PG_VERSION}/include/ || true) && \
    cp -rn /tmp/postgres-include/* /usr/lib/postgresql/${PG_VERSION}/include/ && \
    rm -rf /tmp/postgres-include

COPY --from=build-image /usr/lib/postgresql/${PG_VERSION}/share/ /tmp/postgres-share/
RUN (mkdir -p /usr/lib/postgresql/${PG_VERSION}/share/ || true) && \
    cp -rn /tmp/postgres-share/* /usr/lib/postgresql/${PG_VERSION}/share/ && \
    rm -rf /tmp/postgres-share

COPY --from=build-image /usr/lib/postgresql/${PG_VERSION}/share/extension /tmp/postgres-extension/
RUN (mkdir -p /usr/share/postgresql/${PG_VERSION}/extension/ || true) && \
    cp -rn /tmp/postgres-extension/* /usr/share/postgresql/${PG_VERSION}/extension/ && \
    rm -rf /tmp/postgres-extension

COPY --from=build-image /usr/lib/x86_64-linux-gnu /tmp/x86_64-linux-gnu/
RUN cp -rn /tmp/x86_64-linux-gnu/* /usr/lib/x86_64-linux-gnu && \
    rm -rf /tmp/x86_64-linux-gnu/
# locale
RUN rm -rf /var/lib/apt/lists/* \
    && localedef -i en_US -c -f UTF-8 -A /usr/share/locale/locale.alias en_US.UTF-8

ENV LANG en_US.UTF-8
ENV LC_ALL en_US.UTF-8
ENV LANGUAGE en_US

# Create documentdb user
RUN useradd -ms /bin/bash documentdb -G sudo
RUN echo "%sudo ALL=(ALL:ALL) NOPASSWD: ALL" >> /etc/sudoers.d/no-pass-ask

# Add postgres to path for sudo commands.
# Add path for sudoers
RUN cat /etc/sudoers | grep secure_path | sed "s/\:\/bin\:/\:\/bin\:\/usr\/lib\/postgresql\/$PG_VERSION\/bin\:/" >> /etc/sudoers.d/postgres_path

# Add PG to the path
ENV PATH=$PATH:/usr/lib/postgresql/$PG_VERSION/bin

COPY --from=build-image /home/documentdb/code/scripts/* /home/documentdb/scripts/

USER documentdb
WORKDIR /home/documentdb

ENTRYPOINT ["/bin/bash", "-c", "/home/documentdb/scripts/start_oss_server.sh -x true && exec bash"]
