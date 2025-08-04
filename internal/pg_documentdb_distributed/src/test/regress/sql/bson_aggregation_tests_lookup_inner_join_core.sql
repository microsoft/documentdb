SET search_path TO documentdb_core,documentdb_api,documentdb_api_catalog,documentdb_api_internal;

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
set local documentdb.forceBitmapScanForLookup to off;
set local documentdb.enableLookupInnerJoin to off;
SELECT document FROM bson_aggregation_pipeline('lookupdb', 
    '{ "aggregate": "planes", "pipeline": [ { "$match": { "model": { "$exists": true } } }, { "$lookup": { "from": "gate_availability", "as": "matched_docs", "localField": "model", "foreignField": "plane_model" } }, { "$unwind": "$matched_docs" } ], "cursor": {} }');
set local documentdb.enableLookupInnerJoin to on;
SELECT document FROM bson_aggregation_pipeline('lookupdb', 
    '{ "aggregate": "planes", "pipeline": [ { "$match": { "model": { "$exists": true } } }, { "$lookup": { "from": "gate_availability", "as": "matched_docs", "localField": "model", "foreignField": "plane_model" } }, { "$unwind": "$matched_docs" } ], "cursor": {} }');

set local documentdb.forceBitmapScanForLookup to on;
set local documentdb.enableLookupInnerJoin to off;
SELECT document FROM bson_aggregation_pipeline('lookupdb', 
    '{ "aggregate": "planes", "pipeline": [ { "$match": { "model": { "$exists": true } } }, { "$lookup": { "from": "gate_availability", "as": "matched_docs", "localField": "model", "foreignField": "plane_model" } }, { "$unwind": "$matched_docs" } ], "cursor": {} }');
set local documentdb.enableLookupInnerJoin to on;
SELECT document FROM bson_aggregation_pipeline('lookupdb', 
    '{ "aggregate": "planes", "pipeline": [ { "$match": { "model": { "$exists": true } } }, { "$lookup": { "from": "gate_availability", "as": "matched_docs", "localField": "model", "foreignField": "plane_model" } }, { "$unwind": "$matched_docs" } ], "cursor": {} }');
ROLLBACK;

-- Insert a lot more data
DO $$
DECLARE i int;
BEGIN
FOR i IN 1..1000 LOOP
PERFORM documentdb_api.insert_one('lookupdb','planes',' { "model" : "A380", "price" : 280, "quantity" : 20 }', NULL);
PERFORM documentdb_api.insert_one('lookupdb','planes','{ "model" : "A340", "price" : 140, "quantity" : 1 }', NULL);
PERFORM documentdb_api.insert_one('lookupdb','planes',' { "model" : "A330", "price" : 10, "quantity" : 5 }', NULL);
PERFORM documentdb_api.insert_one('lookupdb','planes',' { "model" : "737", "price" : 50, "quantity" : 30 }', NULL);
END LOOP;
END;
$$;

DO $$
DECLARE i int;
BEGIN
FOR i IN 1..250 LOOP
PERFORM documentdb_api.insert_one('lookupdb','gate_availability',' { "plane_model" : "A330", "gates" : 30 }', NULL);
PERFORM documentdb_api.insert_one('lookupdb','gate_availability',' { "plane_model" : "A340", "gates" : 10 }', NULL);
PERFORM documentdb_api.insert_one('lookupdb','gate_availability','{ "plane_model" : "A380", "gates" : 5 }', NULL);
PERFORM documentdb_api.insert_one('lookupdb','gate_availability','{ "plane_model" : "A350", "gates" : 20 }', NULL);
PERFORM documentdb_api.insert_one('lookupdb','gate_availability','{ "plane_model" : "737", "gates" : 110 }', NULL);
END LOOP;
END;
$$;

-- Now test index usage
BEGIN;

set local documentdb.forceBitmapScanForLookup to on;
set local documentdb.enableLookupInnerJoin to off;
-- LEFT JOIN with force bitmap scan, should use materialize seq scan
EXPLAIN (SUMMARY OFF, COSTS OFF) SELECT document FROM bson_aggregation_pipeline('lookupdb', 
    '{ "aggregate": "planes", "pipeline": [ { "$match": { "model": { "$exists": true } } }, { "$lookup": { "from": "gate_availability", "as": "matched_docs", "localField": "model", "foreignField": "plane_model" } }, { "$unwind": "$matched_docs" } ], "cursor": {} }');

set local documentdb.enableLookupInnerJoin to on;
-- RIGHT JOIN with force bitmap scan, should use materialize seq scan
EXPLAIN (SUMMARY OFF, COSTS OFF) SELECT document FROM bson_aggregation_pipeline('lookupdb', 
    '{ "aggregate": "planes", "pipeline": [ { "$match": { "model": { "$exists": true } } }, { "$lookup": { "from": "gate_availability", "as": "matched_docs", "localField": "model", "foreignField": "plane_model" } }, { "$unwind": "$matched_docs" } ], "cursor": {} }');

set local documentdb.forceBitmapScanForLookup to off;
set local documentdb.enableLookupInnerJoin to off;

-- LEFT JOIN without force bitmap scan, should use index scan
EXPLAIN (SUMMARY OFF, COSTS OFF) SELECT document FROM bson_aggregation_pipeline('lookupdb', 
    '{ "aggregate": "planes", "pipeline": [ { "$match": { "model": { "$exists": true } } }, { "$lookup": { "from": "gate_availability", "as": "matched_docs", "localField": "model", "foreignField": "plane_model" } }, { "$unwind": "$matched_docs" } ], "cursor": {} }');

set local documentdb.enableLookupInnerJoin to on;

-- RIGHT JOIN without force bitmap scan, should use index scan
EXPLAIN (SUMMARY OFF, COSTS OFF) SELECT document FROM bson_aggregation_pipeline('lookupdb', 
    '{ "aggregate": "planes", "pipeline": [ { "$match": { "model": { "$exists": true } } }, { "$lookup": { "from": "gate_availability", "as": "matched_docs", "localField": "model", "foreignField": "plane_model" } }, { "$unwind": "$matched_docs" } ], "cursor": {} }');

ROLLBACK;

-- Cleanup
SELECT documentdb_api.drop_collection('lookupdb', 'planes');
SELECT documentdb_api.drop_collection('lookupdb', 'gate_availability');
