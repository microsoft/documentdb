# 🛠️ To Build Your Own Debian Packages With Docker

Run `./packaging/build_packages.sh -h` and follow the instructions.
E.g. to build for Debian 12 and PostgreSQL 16, run:

```sh
./packaging/build_packages.sh --os deb12 --pg 16
```

Packages can be found at the `packages` directory by default, but it can be configured with the `--output-dir` option.

**Note:** The packages do not include pg_documentdb_distributed in the `internal` directory.

# 📦 Installing DocumentDB from `.deb` Packages

We provide prebuilt `.deb` packages for PostgreSQL versions **15**, **16**, and **17** on the following operating systems:

- **Debian 11** (bullseye)
- **Debian 12** (bookworm)
- **Ubuntu 22.04** (jammy)
- **Ubuntu 24.04** (noble)

## 🔽 1. Download the Package

Visit the [Releases](../../releases) page of this repository and download the `.deb` package that matches your OS and PostgreSQL version.

Example:

```bash
wget https://github.com/YOUR_ORG/YOUR_REPO/releases/download/v0.103.0/deb12-postgresql-16-documentdb_0.103.0_amd64.deb
```

## 🔽 2. Install the Package

Install the downloaded package using `dpkg`:
```bash
sudo dpkg -i deb12-postgresql-16-documentdb_0.103.0_amd64.deb
```
If you see dependency errors, fix them with:
```bash
sudo apt-get install -f
```

## 🔽 3. Enable the Extension in PostgreSQL

After installing the package, enable the extension in your PostgreSQL database:
```sql
CREATE EXTENSION documentdb;
```
Make sure you connect to the correct PostgreSQL instance that matches the installed version.

## 🔽 4. Verify Installation
Run the following query to verify that the extension is available:

```sql
SELECT * FROM pg_available_extensions WHERE name = 'documentdb';
```