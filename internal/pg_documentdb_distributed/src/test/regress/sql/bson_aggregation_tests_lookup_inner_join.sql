SET search_path TO documentdb_core,documentdb_api,documentdb_api_catalog,documentdb_api_internal;

SET citus.next_shard_id TO 731000;
SET documentdb.next_collection_id TO 7310;
SET documentdb.next_collection_index_id TO 7310;

-- Insert data
SELECT documentdb_api.insert_one('lookupdb','planes',' { "_id" : 1, "model" : "A380", "price" : 280, "quantity" : 20 }', NULL);
SELECT documentdb_api.insert_one('lookupdb','planes','{ "_id" : 2, "model" : "A340", "price" : 140, "quantity" : 1 }', NULL);
SELECT documentdb_api.insert_one('lookupdb','planes',' { "_id" : 3, "model" : "A330", "price" : 10, "quantity" : 5 }', NULL);
SELECT documentdb_api.insert_one('lookupdb','planes',' { "_id" : 4, "model" : "737", "price" : 50, "quantity" : 30 }', NULL);

SELECT documentdb_api.insert_one('lookupdb','gate_availability',' { "_id" : 1, "plane_model" : "A330", "gates" : 30 }', NULL);
SELECT documentdb_api.insert_one('lookupdb','gate_availability',' { "_id" : 11, "plane_model" : "A340", "gates" : 10 }', NULL);
SELECT documentdb_api.insert_one('lookupdb','gate_availability','{ "_id" : 2, "plane_model" : "A380", "gates" : 5 }', NULL);
SELECT documentdb_api.insert_one('lookupdb','gate_availability','{ "_id" : 3, "plane_model" : "A350", "gates" : 20 }', NULL);
SELECT documentdb_api.insert_one('lookupdb','gate_availability','{ "_id" : 4, "plane_model" : "737", "gates" : 110 }', NULL);


-- set up indexes
SELECT documentdb_api_internal.create_indexes_non_concurrently('lookupdb', '{ "createIndexes": "planes", "indexes": [ { "key": { "model": 1 }, "name": "planes_model_1" } ] }', TRUE);
SELECT documentdb_api_internal.create_indexes_non_concurrently('lookupdb', '{ "createIndexes": "gate_availability", "indexes": [ { "key": { "plane_model": 1 }, "name": "plane_model_1" } ] }', TRUE);

-- Remove primary key
ALTER TABLE documentdb_data.documents_7311 DROP CONSTRAINT collection_pk_7311;
ALTER TABLE documentdb_data.documents_7312 DROP CONSTRAINT collection_pk_7312;

ANALYZE documentdb_data.documents_7311;
ANALYZE documentdb_data.documents_7312;
BEGIN;
set local enable_seqscan to off;
set local documentdb.enableLookupInnerJoin to on;
SELECT document FROM bson_aggregation_pipeline('lookupdb', 
    '{ "aggregate": "planes", "pipeline": [ { "$match": { "model": { "$exists": true } } }, { "$lookup": { "from": "gate_availability", "as": "matched_docs", "localField": "model", "foreignField": "plane_model" } }, { "$unwind": "$matched_docs" } ], "cursor": {} }');
ROLLBACK;