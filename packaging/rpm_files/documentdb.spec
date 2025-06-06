%global pg_version POSTGRES_VERSION
%define debug_package %{nil}

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

%description
DocumentDB is the open-source engine powering vCore-based Azure Cosmos DB for MongoDB. 
It offers a native implementation of document-oriented NoSQL database, enabling seamless 
CRUD operations on BSON data types within a PostgreSQL framework.

%prep
%setup -q

%build
# Keep the internal directory out of the RPM package
sed -i '/internal/d' Makefile

# Build the extension
# Ensure PG_CONFIG points to the correct pg_config for PGDG paths
make %{?_smp_mflags} PG_CONFIG=/usr/pgsql-%{pg_version}/bin/pg_config PG_CFLAGS="-std=gnu99 -Wall -Wno-error" CFLAGS=""

%install
make install DESTDIR=%{buildroot}

# Remove the bitcode directory if it's not needed in the final package
rm -rf %{buildroot}/usr/pgsql-%{pg_version}/lib/bitcode

%files
%defattr(-,root,root,-)
/usr/pgsql-%{pg_version}/lib/pg_documentdb_core.so
/usr/pgsql-%{pg_version}/lib/pg_documentdb.so
/usr/pgsql-%{pg_version}/share/extension/documentdb_core.control
/usr/pgsql-%{pg_version}/share/extension/documentdb_core--*.sql
/usr/pgsql-%{pg_version}/share/extension/documentdb.control
/usr/pgsql-%{pg_version}/share/extension/documentdb--*.sql

%changelog
* Thu May 29 2025 Shuai Tian <shuaitian@microsoft.com> - DOCUMENTDB_VERSION-1
- Initial RPM package for DocumentDB