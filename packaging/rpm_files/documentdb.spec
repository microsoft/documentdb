%global pg_version POSTGRES_VERSION

Name:           postgresql%{pg_version}-documentdb
Version:        DOCUMENTDB_VERSION
Release:        1%{?dist}
Summary:        DocumentDB is the open-source engine powering vCore-based Azure Cosmos DB for MongoDB

License:        MIT
URL:            https://github.com/microsoft/documentdb
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  cmake
BuildRequires:  postgresql%{pg_version}-devel
BuildRequires:  libicu-devel
BuildRequires:  krb5-devel
BuildRequires:  pkg-config

Requires:       postgresql%{pg_version}
Requires:       postgresql%{pg_version}-server
Requires:       postgresql%{pg_version}-contrib

%description
DocumentDB is the open-source engine powering vCore-based Azure Cosmos DB for MongoDB. 
It offers a native implementation of document-oriented NoSQL database, enabling seamless 
CRUD operations on BSON data types within a PostgreSQL framework.

%prep
%setup -q

%build
# Keep the internal directory out of the RPM package
sed -i '/internal/d' Makefile

# Build the extension with relaxed warning flags for cross-compilation
# Override all warning-related flags
make %{?_smp_mflags} PG_CFLAGS="-std=gnu99 -Wall -Wno-error" CFLAGS=""

%install
# Build with Ubuntu paths but install to RHEL-compatible paths
make install DESTDIR=%{buildroot}

# Create RHEL-style directory structure
mkdir -p %{buildroot}/usr/pgsql-%{pg_version}/lib
mkdir -p %{buildroot}/usr/pgsql-%{pg_version}/share/extension

# Move files from Ubuntu paths to RHEL paths
# Ubuntu uses /usr/lib/postgresql/XX/lib/ and /usr/share/postgresql/XX/extension/
# RHEL uses /usr/pgsql-XX/lib/ and /usr/pgsql-XX/share/extension/
if [ -d %{buildroot}/usr/lib/postgresql/%{pg_version}/lib ]; then
    mv %{buildroot}/usr/lib/postgresql/%{pg_version}/lib/* %{buildroot}/usr/pgsql-%{pg_version}/lib/
fi
if [ -d %{buildroot}/usr/share/postgresql/%{pg_version}/extension ]; then
    mv %{buildroot}/usr/share/postgresql/%{pg_version}/extension/* %{buildroot}/usr/pgsql-%{pg_version}/share/extension/
fi

# Clean up Ubuntu-style directories
rm -rf %{buildroot}/usr/lib/postgresql
rm -rf %{buildroot}/usr/share/postgresql

%files
%defattr(-,root,root,-)
/usr/pgsql-%{pg_version}/lib/documentdb_core.so
/usr/pgsql-%{pg_version}/lib/pg_documentdb.so
/usr/pgsql-%{pg_version}/share/extension/documentdb_core.control
/usr/pgsql-%{pg_version}/share/extension/documentdb_core--*.sql
/usr/pgsql-%{pg_version}/share/extension/pg_documentdb.control
/usr/pgsql-%{pg_version}/share/extension/pg_documentdb--*.sql

%changelog
* Thu May 29 2025 Shuai Tian <shuaitian@microsoft.com> - DOCUMENTDB_VERSION-1
- Initial RPM package for DocumentDB