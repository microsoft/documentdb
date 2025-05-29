%global pg_version POSTGRES_VERSION

Name:           postgresql%{pg_version}-documentdb
Version:        DOCUMENTDB_VERSION
Release:        1%{?dist}
Summary:        DocumentDB is the open-source engine powering vCore-based Azure Cosmos DB for MongoDB

License:        MIT
URL:            https://github.com/microsoft/documentdb
Source0:        documentdb-%{version}.tar.gz

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
%setup -q -n documentdb-%{version}

%build
# Keep the internal directory out of the RPM package
sed -i '/internal/d' Makefile

# Build the extension
make %{?_smp_mflags}

%install
make install DESTDIR=%{buildroot}

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