# Introduction

`DocumentDB` is the engine powering vCore-based Azure Cosmos DB for MongoDB. It offers a native implementation of document-oriented NoSQL database, enabling seamless CRUD operations on BSON data types within a PostgreSQL framework. Beyond basic operations, DocumentDB empowers you to execute complex workloads, including full-text searches, geospatial queries, and vector embeddings on your dataset, delivering robust functionality and flexibility for diverse data management needs.

[PostgreSQL](https://www.postgresql.org/about/) is a powerful, open source object-relational database system that uses and extends the SQL language combined with many features that safely store and scale the most complicated data workloads.

## Components

The project comprises of two primary components, which work together to support document operations.

- **pg_documentdb_core :** PostgreSQL extension introducing BSON datatype support and operations for native Postgres.
- **pg_documentdb :** The public API surface for DocumentDB providing CRUD functionality on documents in the store.


## Why DocumentDB ?

At DocumentDB, we believe in the power of open-source to drive innovation and collaboration. Our commitment to being a fully open-source document database means that we are dedicated to transparency, community involvement, and continuous improvement. We are open-sourced under the most permissive [MIT](https://opensource.org/license/mit) license, where developers and organizations alike have no restrictions incorporating the project into new and existing solutions of their own. DocumentDB introduces the BSON data type and provides APIs for seamless operation within native PostgreSQL, enhancing efficiency and aligning with operational advantages.

DocumentDB also provides a powerful on-premise solution, allowing organizations to maintain full control over their data and infrastructure. This flexibility ensures that you can deploy it in your own environment, meeting your specific security, compliance, and performance requirements. With DocumentDB, you get the best of both worlds: the innovation of open-source and the control of on-premise deployment.

### Based on Postgres

DocumentDB is built on top of PostgreSQL, one of the most advanced and reliable open-source relational database systems available. We chose PostgreSQL as our base layer for several reasons:

1. **Proven Stability and Performance**: PostgreSQL has a long history of stability and performance, making it a trusted choice for mission-critical applications.
2. **Extensibility**: Their extensible architecture allows us to integrate a DocumentDB API on BSON data type seamlessly, providing the flexibility to handle both relational and document data.
3. **Active Community**: PostgreSQL has a vibrant and active community that continuously contributes to its development, ensuring that it remains at the forefront of database technology.
4. **Advanced Features**: PostgreSQL offers a rich set of features, including advanced indexing, full-text search, and powerful querying capabilities, which enhance the functionality of DocumentDB.
5. **Compliance and Security**: PostgreSQL's robust security features and compliance with various standards make it an ideal choice for organizations with stringent security and regulatory requirements.

By building on PostgreSQL, DocumentDB leverages these strengths to provide a powerful, flexible, and reliable document database that meets the need of modern applications. DocumentDB will continue to benefit from the advancements brought into the PostgreSQL ecosystem.

## Quick Links

**Documentation & Getting Started**
- [Full Documentation](https://microsoft.github.io/documentdb/v1/) - Complete documentation site
- [Getting Started Guide](https://microsoft.github.io/documentdb/v1/get-started/) - Quick setup and installation
- [Why DocumentDB?](https://microsoft.github.io/documentdb/v1/why-documentdb/) - Learn about DocumentDB's advantages

**Core Features & Usage**
- [CRUD Operations](https://microsoft.github.io/documentdb/v1/usage/) - Basic database operations
- [Collection Management](https://microsoft.github.io/documentdb/v1/collection-management/) - Managing collections
- [Indexing](https://microsoft.github.io/documentdb/v1/indexing/) - Performance optimization
- [Aggregation Framework](https://microsoft.github.io/documentdb/v1/aggregation/) - Advanced data processing

**Advanced Topics**
- [DocumentDB Gateway](https://microsoft.github.io/documentdb/v1/gateway/) - Gateway configuration
- [Joins](https://microsoft.github.io/documentdb/v1/joins/) - Cross-collection operations
- [Packaging](https://microsoft.github.io/documentdb/v1/packaging/) - Deployment options
- [Prebuild Images](https://microsoft.github.io/documentdb/v1/prebuild/) - Docker images