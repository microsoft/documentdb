# DocumentDB

![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)
![Release](https://img.shields.io/github/v/release/microsoft/documentdb)
![Discord](https://img.shields.io/discord/1374170121219866635?label=Discord\&logo=discord)

## Overview

**DocumentDB** is an open-source, distributed document database engine offering native support for the MongoDB wire protocol. It integrates with PostgreSQL for metadata and control, enabling distributed query processing, sharding, and cloud-native durability.

DocumentDB supports advanced data workloads like full-text search, geospatial queries, and vector embeddings, offering a powerful yet flexible development experience.

* Website: [https://documentdb.io](https://documentdb.io)
* Docs: [https://github.com/documentdb/docs](https://github.com/documentdb/docs)
* Community: [Join Discord](https://discord.gg/vH7bYu524D)

---

## Why DocumentDB?

* MIT-licensed and 100% open-source
* Built on upstream PostgreSQL — no forks
* MongoDB protocol compatibility
* Local or cloud-native deployments
* Supports advanced search and analytics
* Transparent governance and community development ([GOVERNANCE.md](GOVERNANCE.md))

---

## Quickstart

### Prerequisites

* Docker
* Linux, macOS (dev only), or WSL2
* [`mongosh`](https://github.com/mongodb-js/mongosh)

### Option 1 (recommended): using Docker

```bash
docker pull ghcr.io/microsoft/documentdb/documentdb-local:latest
docker tag ghcr.io/microsoft/documentdb/documentdb-local:latest documentdb
docker run -dt -p 10260:10260 --name documentdb-container documentdb --username <YOUR_USERNAME> --password <YOUR_PASSWORD>
docker image rm -f ghcr.io/microsoft/documentdb/documentdb-local:latest || echo "No existing documentdb image to remove"
```

### Option 2: Build from source

```bash
git clone https://github.com/microsoft/documentdb.git
cd documentdb
./scripts/dev-container/start.sh
```

---

## Hello World

### Python
```python
import pymongo
from datetime import datetime

# Connect to DocumentDB
client = pymongo.MongoClient('mongodb://localhost:10260')
db = client.test_database
collection = db.test_collection

# Insert a document
collection.insert_one({
    'name': 'DocumentDB',
    'type': 'engine',
    'created_at': datetime.utcnow()
})

# Query documents
results = collection.find({'type': 'engine'})
for doc in results:
    print(doc)
```

### Node.js
```javascript
const { MongoClient } = require('mongodb');

async function helloWorld() {
  const uri = 'mongodb://localhost:10260';
  const client = new MongoClient(uri);

  try {
    await client.connect();
    const db = client.db('test_database');
    const collection = db.collection('test_collection');

    // Insert a document
    await collection.insertOne({
      name: 'DocumentDB',
      type: 'engine',
      created_at: new Date()
    });

    // Query documents
    const results = await collection.find({ type: 'engine' }).toArray();
    console.log('Documents found:', results);

  } catch (error) {
    console.error('Error:', error);
  } finally {
    await client.close();
  }
}

helloWorld();
```


### MongoDB Shell
```bash
mongosh "mongodb://localhost:10260"
db.test.insertOne({ name: "DocumentDB", type: "engine" })
```
---

## Usage

* **MongoDB-compatible store** - Use existing MongoDB applications
* **Distributed query execution** - Scale across multiple nodes
* **MongoDB shell, drivers, or ORMs** - Full ecosystem compatibility
* **Search/vector workloads** - AI/ML applications
* **Hybrid data models** - Combine document and relational data

### Connection Examples
```python
# Basic connection
client = pymongo.MongoClient('mongodb://localhost:10260')

# With authentication
client = pymongo.MongoClient('mongodb://username:password@localhost:10260')

# With connection pooling
client = pymongo.MongoClient(
    'mongodb://localhost:10260',
    maxPoolSize=50,
    retryWrites=False
)
```

### Basic Operations
```python
# Insert documents
collection.insert_one({
    'name': 'John Doe',
    'email': 'john@example.com',
    'created_at': datetime.utcnow()
})

# Find documents
result = collection.find({'name': 'John Doe'})

# Update documents
collection.update_one(
    {'name': 'John Doe'},
    {'$set': {'status': 'active'}}
)

# Delete documents
collection.delete_one({'name': 'John Doe'})
```


## Advanced Features

* **Full-text search** - Built-in text search capabilities
* **Geospatial queries** - Location-based data querying
* **Vector similarity search** - AI/ML embedding support
* **Aggregation framework** - Complex data processing
* **Bulk operations** - High-performance batch operations
* **PostgreSQL integration** - Direct SQL access to BSON documents

### Vector Search Example
```python
# Vector similarity search
results = collection.find({
    '$vectorSearch': {
        'queryVector': [0.1, 0.2, 0.3],
        'path': 'embeddings',
        'numCandidates': 100,
        'limit': 10
    }
})
```

### Aggregation Example
```python
pipeline = [
    {'$match': {'status': 'active'}},
    {'$group': {
        '_id': '$type',
        'count': {'$sum': 1},
        'avg_value': {'$avg': '$value'}
    }}
]
results = collection.aggregate(pipeline)
```

---

## Troubleshooting

### Common Issues
```python
# Connection error handling
try:
    client = pymongo.MongoClient(connection_string)
    client.admin.command('ping')
except pymongo.errors.ConnectionError as e:
    print(f"Connection error: {e}")

# Operation error handling
from pymongo.errors import OperationFailure
try:
    result = collection.insert_one({'_id': existing_id})
except OperationFailure as e:
    print(f"Operation failed: {e}")
```

### Best Practices
```python
# Configure connection pool
client = pymongo.MongoClient(
    connection_string,
    maxPoolSize=50,
    waitQueueTimeoutMS=2000
)

# Always close connections when done
try:
    # Your code here
finally:
    client.close()
```

---

## Development & Contribution

* [Contribution guide](CONTRIBUTING.md)
* [Governance](GOVERNANCE.md)

---

## Documentation & Support

* Website: [https://documentdb.io](https://documentdb.io)
* Community chat: [Discord](https://discord.gg/vH7bYu524D)
* Roadmap: GitHub Projects
* Ask questions: GitHub Discussions
* Report issues: GitHub Issues
* Releases: [CHANGELOG.md](CHANGELOG.md)

---

## FAQ

**Q: What OS/platforms are supported?**
A: Linux (Ubuntu 20.04+), macOS (dev only), WSL2, Docker

**Q: Can I use MongoDB drivers?**
A: Yes, DocumentDB is MongoDB wire protocol-compatible.

**Q: Can I use it in production?**
A: It is under active development — production-readiness depends on your requirements.

**Q: Where’s the roadmap?**
A: See GitHub Projects or [documentdb.io](https://documentdb.io).

---

## License

* MIT License ([LICENSE](LICENSE))
* Contributions welcome!
