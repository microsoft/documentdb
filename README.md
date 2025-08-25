# Introduction

`DocumentDB` is a MongoDB compatible open source document database built on PostgreSQL. It offers a native implementation of a document-oriented NoSQL database, enabling seamless CRUD (Create, Read, Update, Delete) operations on BSON(Binary JSON) data types within a PostgreSQL framework. Beyond basic operations, DocumentDB empowers users to execute complex workloads, including full-text searches, geospatial queries, and vector search, delivering robust functionality and flexibility for diverse data management needs.

[PostgreSQL](https://www.postgresql.org/about/) is a powerful, open source object-relational database system that uses and extends the SQL language combined with many features that safely store and scale the most complicated data workloads.

## Components

The project comprises of three components, which work together to support document operations.

- **pg_documentdb_core :** PostgreSQL extension introducing BSON datatype support and operations for native Postgres.
- **pg_documentdb :** The public API surface for DocumentDB providing CRUD functionality on documents in the store.
- **pg_documentdb_gw :** The gateway protocol translation layer that converts the user's MongoDB APIs into PostgreSQL queries.


## Why DocumentDB ?

At DocumentDB, we believe in the power of open-source to drive innovation and collaboration. Our commitment to being a fully open-source MongoDB compatible document database means that we are dedicated to transparency, community involvement, and continuous improvement. We are open-sourced under the most permissive [MIT](https://opensource.org/license/mit) license, where developers and organizations alike have no restrictions incorporating the project into new and existing solutions of their own. DocumentDB introduces the BSON data type to PostgreSQL and provides APIs for seamless operation within native PostgreSQL, enhancing efficiency and aligning with operational advantages.

DocumentDB also provides a powerful on-premise solution, allowing organizations to maintain full control over their data and infrastructure. This flexibility ensures that you can deploy it in your own environment, meeting your specific security, compliance, and performance requirements. With DocumentDB, you get the best of both worlds: the innovation of open-source and the control of on-premise deployment.

### Based on Postgres

We chose PostgreSQL as our platform for several reasons:

1. **Proven Stability and Performance**: PostgreSQL has a long history of stability and performance, making it a trusted choice for mission-critical applications.
2. **Extensibility**: The extensible architecture of PostgreSQL allows us to integrate a DocumentDB API on BSON data type seamlessly, providing the flexibility to handle both relational and document data.
3. **Active Community**: PostgreSQL has a vibrant and active community that continuously contributes to its development, ensuring that it remains at the forefront of database technology.
4. **Advanced Features**: PostgreSQL offers a rich feature set, including advanced indexing, full-text search, and powerful querying capabilities, which enhance the functionality of DocumentDB.
5. **Compliance and Security**: PostgreSQL's robust security features and compliance with various standards makes it an ideal choice for organizations with stringent security and regulatory requirements.

## Get Started

### Prerequisites
- Python 3.7+
- pip package manager
- Docker
- Git (for cloning the repository)

Step 1: Install Python

```bash

pip install pymongo

```

Step 2. Install optional dependencies

```bash

pip install dnspython

```

Step 3. Setup DocumentDB using Docker

```bash

   # Pull the latest DocumentDB Docker image
   docker pull ghcr.io/microsoft/documentdb/documentdb-local:latest

   # Tag the image for convenience
   docker tag ghcr.io/microsoft/documentdb/documentdb-local:latest documentdb

   # Run the container with your chosen username and password
   docker run -dt -p 10260:10260 --name documentdb-container documentdb --username <YOUR_USERNAME> --password <YOUR_PASSWORD>
   docker image rm -f ghcr.io/microsoft/documentdb/documentdb-local:latest || echo "No existing documentdb image to remove"

```

> **Note:** During the transition to the Linux Foundation, Docker images may still be hosted on Microsoft's container registry. These will be migrated to the new DocumentDB organization as the transition completes.
   > **Note:** Replace `<YOUR_USERNAME>` and `<YOUR_PASSWORD>` with your desired credentials. You must set these when creating the container for authentication to work.
   > 
   > **Port Note:** Port `10260` is used by default in these instructions to avoid conflicts with other local database services. You can use port `27017` (the standard MongoDB port) or any other available port if you prefer. If you do, be sure to update the port number in both your `docker run` command and your connection string accordingly.

Step 4: Initialize the pymongo client with the credentials from the previous step

```python

import pymongo

from pymongo import MongoClient

# Create a MongoDB client and open a connection to DocumentDB
client = pymongo.MongoClient(
    'mongodb://<YOUR_USERNAME>:<YOUR_PASSWORD>@localhost:10260/?tls=true&tlsAllowInvalidCertificates=true'
)

```

Step 5: Create a database and collection

```python

quickStartDatabase = client["quickStartDatabase"]
quickStartCollection = quickStartDatabase.create_collection("quickStartCollection")

```

Step 6: Insert documents

```python

# Insert a single document
quickStartCollection.insert_one({
       'name': 'John Doe',
       'email': 'john@email.com',
       'address': '123 Main St, Anytown, USA',
       'phone': '555-1234'
   })

# Insert multiple documents
quickStartCollection.insert_many([
    {
        'name': 'Jane Smith',
        'email': 'jane@email.com',
        'address': '456 Elm St, Othertown, USA',
        'phone': '555-5678'
    },
    {
        'name': 'Alice Johnson',
        'email': 'alice@email.com',
        'address': '789 Oak St, Sometown, USA',
        'phone': '555-8765'
    }
])

```

Step 7: Read documents

```python

# Read all documents
for document in quickStartCollection.find():
    print(document)

# Read a specific document
singleDocumentReadResult = quickStartCollection.find_one({'name': 'John Doe'})
    print(singleDocumentReadResult)

```

Step 8: Run aggregation pipeline query

```python

pipeline = [
    {'$match': {'name': 'Alice Johnson'}},
    {'$project': {
        '_id': 0,
        'name': 1,
        'email': 1
    }}
]

results = quickStartCollection.aggregate(pipeline)
print("Aggregation results:")
for eachDocument in results:
    print(eachDocument)

```

## To interact directly with the PostgreSQL layer

### Pre-requisite

- Ensure [Docker](https://docs.docker.com/engine/install/) is installed on your system.

### Building DocumentDB with Docker

Step 1: Clone the DocumentDB repo.

```bash
git clone https://github.com/microsoft/documentdb.git
```

Step 2: Create the docker image. Navigate to cloned repo.

```bash
docker build . -f .devcontainer/Dockerfile -t documentdb 
```

Note: Validate using `docker image ls`

Step 3: Run the Image as a container

```bash
docker run -v $(pwd):/home/documentdb/code -it documentdb /bin/bash 

cd code
```

(Aligns local location with docker image created, allows de-duplicating cloning repo again within image).<br>
Note: Validate container is running `docker container ls`

Step 4: Build & Deploy the binaries

```bash
make 
```

Note: Run in case of an unsuccessful build `git config --global --add safe.directory /home/documentdb/code` within image.

```bash
sudo make install
```

Note: To run backend postgresql tests after installing you can run `make check`.

You are all set to work with DocumentDB.

### Using the Prebuilt Docker Image

You can use a [prebuilt docker image](https://github.com/microsoft/documentdb/pkgs/container/documentdb%2Fdocumentdb-oss/versions?filters%5Bversion_type%5D=tagged) for DocumentDB instead of building it from source.  Follow these steps:

#### Pull the Prebuilt Image

Pull the prebuilt image directly from the Microsoft Container Registry:

```bash
docker pull ghcr.io/microsoft/documentdb/documentdb-oss:PG16-amd64-0.105.0
```

#### Running the Prebuilt Image

To run the prebuilt image, use one of the following commands:

1. Run the container:

```bash
docker run -dt ghcr.io/microsoft/documentdb/documentdb-oss:PG16-amd64-0.105.0
```

2. If external access is required, run the container with parameter "-e":

```bash
docker run -p 127.0.0.1:9712:9712 -dt ghcr.io/microsoft/documentdb/documentdb-oss:PG16-amd64-0.105.0 -e
```

This will start the container and map port `9712` from the container to the host.

### Connecting to the Server
#### Internal Access
Step 1: Run `start_oss_server.sh` to initialize the DocumentDB server and manage dependencies.

```bash
./scripts/start_oss_server.sh
```

Or logging into the container if using prebuild image
```bash
docker exec -it <container-id> bash
```

Step 2: Connect to `psql` shell

```bash
psql -p 9712 -d postgres
```

#### External Access
Connect to `psql` shell

```bash
psql -h localhost --port 9712 -d postgres -U documentdb
```

## Usage directly through the PostgreSQL layer

Once you have your `DocumentDB` set up running, you can start with creating collections, indexes and perform queries on them.

### Create a collection

DocumentDB provides [documentdb_api.create_collection](https://github.com/microsoft/documentdb/wiki/Functions#create_collection) function to create a new collection within a specified database, enabling you to manage and organize your BSON documents effectively.

```sql
SELECT documentdb_api.create_collection('documentdb','patient');
```

### Perform CRUD operations

#### Insert documents

The [documentdb_api.insert_one](https://github.com/microsoft/documentdb/wiki/Functions#insert_one) command is used to add a single document into a collection.

```sql
select documentdb_api.insert_one('documentdb','patient', '{ "patient_id": "P001", "name": "Alice Smith", "age": 30, "phone_number": "555-0123", "registration_year": "2023","conditions": ["Diabetes", "Hypertension"]}');
select documentdb_api.insert_one('documentdb','patient', '{ "patient_id": "P002", "name": "Bob Johnson", "age": 45, "phone_number": "555-0456", "registration_year": "2023", "conditions": ["Asthma"]}');
select documentdb_api.insert_one('documentdb','patient', '{ "patient_id": "P003", "name": "Charlie Brown", "age": 29, "phone_number": "555-0789", "registration_year": "2024", "conditions": ["Allergy", "Anemia"]}');
select documentdb_api.insert_one('documentdb','patient', '{ "patient_id": "P004", "name": "Diana Prince", "age": 40, "phone_number": "555-0987", "registration_year": "2024", "conditions": ["Migraine"]}');
select documentdb_api.insert_one('documentdb','patient', '{ "patient_id": "P005", "name": "Edward Norton", "age": 55, "phone_number": "555-1111", "registration_year": "2025", "conditions": ["Hypertension", "Heart Disease"]}');
```

#### Read document from a collection

The `documentdb_api.collection` function is used for retrieving the documents in a collection.

```sql
SELECT document FROM documentdb_api.collection('documentdb','patient');
```

Alternatively, we can apply filter to our queries.

```sql
SET search_path TO documentdb_api, documentdb_core;
SET documentdb_core.bsonUseEJson TO true;

SELECT cursorPage FROM documentdb_api.find_cursor_first_page('documentdb', '{ "find" : "patient", "filter" : {"patient_id":"P005"}}');
```

We can perform range queries as well.

```sql
SELECT cursorPage FROM documentdb_api.find_cursor_first_page('documentdb', '{ "find" : "patient", "filter" : { "$and": [{ "age": { "$gte": 10 } },{ "age": { "$lte": 35 } }] }}');
```

#### Update document in a collection

DocumentDB uses the [documentdb_api.update](https://github.com/microsoft/documentdb/wiki/Functions#update) function to modify existing documents within a collection.

The SQL command updates the `age` for patient `P004`.

```sql
select documentdb_api.update('documentdb', '{"update":"patient", "updates":[{"q":{"patient_id":"P004"},"u":{"$set":{"age":14}}}]}');
```

Similarly, we can update multiple documents using `multi` property.

```sql
SELECT documentdb_api.update('documentdb', '{"update":"patient", "updates":[{"q":{},"u":{"$set":{"age":24}},"multi":true}]}');
```

#### Delete document from the collection

DocumentDB uses the [documentdb_api.delete](https://github.com/microsoft/documentdb/wiki/Functions#delete) function for precise document removal based on specified criteria.

The SQL command deletes the document for patient `P002`.

```sql
SELECT documentdb_api.delete('documentdb', '{"delete": "patient", "deletes": [{"q": {"patient_id": "P002"}, "limit": 1}]}');
```

### Collection management

We can review for the available collections and databases by querying [documentdb_api.list_collections_cursor_first_page](https://github.com/microsoft/documentdb/wiki/Functions#list_collections_cursor_first_page).

```sql
SELECT * FROM documentdb_api.list_collections_cursor_first_page('documentdb', '{ "listCollections": 1 }');
```

[documentdb_api.list_indexes_cursor_first_page](https://github.com/microsoft/documentdb/wiki/Functions#list_indexes_cursor_first_page) allows reviewing for the existing indexes on a collection. We can find collection_id from `documentdb_api.list_collections_cursor_first_page`.

```sql
SELECT documentdb_api.list_indexes_cursor_first_page('documentdb','{"listIndexes": "patient"}');
```

`ttl` indexes by default gets scheduled through the `pg_cron` scheduler, which could be reviewed by querying the `cron.job` table.

```sql
select * from cron.job;
```

### Indexing

#### Create an Index

DocumentDB uses the `documentdb_api.create_indexes_background` function, which allows background index creation without disrupting database operations.

The SQL command demonstrates how to create a `single field` index on `age` on the `patient` collection of the `documentdb`.

```sql
SELECT * FROM documentdb_api.create_indexes_background('documentdb', '{ "createIndexes": "patient", "indexes": [{ "key": {"age": 1},"name": "idx_age"}]}');
```

The SQL command demonstrates how to create a `compound index` on fields age and registration_year on the `patient` collection of the `documentdb`.

```sql
SELECT * FROM documentdb_api.create_indexes_background('documentdb', '{ "createIndexes": "patient", "indexes": [{ "key": {"registration_year": 1, "age": 1},"name": "idx_regyr_age"}]}');
```

#### Drop an Index

DocumentDB uses the `documentdb_api.drop_indexes` function, which allows you to remove an existing index from a collection. The SQL command demonstrates how to drop the index named `id_ab_1` from the `first_collection` collection of the `documentdb`.

```sql
CALL documentdb_api.drop_indexes('documentdb', '{"dropIndexes": "patient", "index":"idx_age"}');
```

### Perform aggregations `Group by`

DocumentDB provides the [documentdb_api.aggregate_cursor_first_page](https://github.com/microsoft/documentdb/wiki/Functions#aggregate_cursor_first_page) function, for performing aggregations over the document store.

The example projects an aggregation on number of patients registered over the years.

```sql
SELECT cursorpage FROM documentdb_api.aggregate_cursor_first_page('documentdb', '{ "aggregate": "patient", "pipeline": [ { "$group": { "_id": "$registration_year", "count_patients": { "$count": {} } } } ] , "cursor": { "batchSize": 3 } }');
```

We can perform more complex operations, listing below a few more usage examples.
The example demonstrates an aggregation on patients, categorizing them into buckets defined by registration_year boundaries.

```sql
SELECT cursorpage FROM documentdb_api.aggregate_cursor_first_page('documentdb', '{ "aggregate": "patient", "pipeline": [ { "$bucket": { "groupBy": "$registration_year", "boundaries": ["2022","2023","2024"], "default": "unknown" } } ], "cursor": { "batchSize": 3 } }');
```

This query performs an aggregation on the `patient` collection to group documents by `registration_year`. It collects unique patient conditions for each registration year using the `$addToSet` operator.

```sql
SELECT cursorpage FROM documentdb_api.aggregate_cursor_first_page('documentdb', '{ "aggregate": "patient", "pipeline": [ { "$group": { "_id": "$registration_year", "conditions": { "$addToSet": { "conditions" : "$conditions" } } } } ], "cursor": { "batchSize": 3 } }');
```

### Join data from multiple collections

Let's create an additional collection named `appointment` to demonstrate how a join operation can be performed.

```sql
select documentdb_api.insert_one('documentdb','appointment', '{"appointment_id": "A001", "patient_id": "P001", "doctor_name": "Dr. Milind", "appointment_date": "2023-01-20", "reason": "Routine checkup" }');
select documentdb_api.insert_one('documentdb','appointment', '{"appointment_id": "A002", "patient_id": "P001", "doctor_name": "Dr. Moore", "appointment_date": "2023-02-10", "reason": "Follow-up"}');
select documentdb_api.insert_one('documentdb','appointment', '{"appointment_id": "A004", "patient_id": "P003", "doctor_name": "Dr. Smith", "appointment_date": "2024-03-12", "reason": "Allergy consultation"}');
select documentdb_api.insert_one('documentdb','appointment', '{"appointment_id": "A005", "patient_id": "P004", "doctor_name": "Dr. Moore", "appointment_date": "2024-04-15", "reason": "Migraine treatment"}');
select documentdb_api.insert_one('documentdb','appointment', '{"appointment_id": "A007","patient_id": "P001", "doctor_name": "Dr. Milind", "appointment_date": "2024-06-05", "reason": "Blood test"}');
select documentdb_api.insert_one('documentdb','appointment', '{ "appointment_id": "A009", "patient_id": "P003", "doctor_name": "Dr. Smith","appointment_date": "2025-01-20", "reason": "Follow-up visit"}');
```

The example presents each patient along with the doctors visited.

```sql
SELECT cursorpage FROM documentdb_api.aggregate_cursor_first_page('documentdb', '{ "aggregate": "patient", "pipeline": [ { "$lookup": { "from": "appointment","localField": "patient_id", "foreignField": "patient_id", "as": "appointment" } },{"$unwind":"$appointment"},{"$project":{"_id":0,"name":1,"appointment.doctor_name":1,"appointment.appointment_date":1}} ], "cursor": { "batchSize": 3 } }');
```

### Community

- Checkout our website at https://documentdb.io to stay up to date with the latest docs and blogs.
- Please refer to page for contributing to our [Roadmap list](https://github.com/orgs/microsoft/projects/1407/views/1).
- [FerretDB](https://github.com/FerretDB/FerretDB) integration allows using DocumentDB as backend engine.
- Contributors and users can join the [DocumentDB Discord channel](https://discord.gg/vH7bYu524D) for quick collaboration.
