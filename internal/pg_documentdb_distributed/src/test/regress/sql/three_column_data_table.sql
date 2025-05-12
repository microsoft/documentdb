SET search_path TO documentdb_core,documentdb_api,documentdb_api_catalog,documentdb_api_internal;

SET citus.next_shard_id TO 2930000;
SET documentdb.next_collection_id TO 293000;
SET documentdb.next_collection_index_id TO 293000;

-- create table with 4 columns first
SELECT documentdb_api.create_collection('db', '4col');

\d documentdb_data.documents_293000
SELECT documentdb_api.insert_one('db', '4col', '{ "_id": 1}');


-- enable GUC
SET documentdb.enableDataTableWithoutCreationTime to on;
-- [1] let's test 4 column table after enabling GUC

-- (1.1)  insert to 4 column collection
SELECT documentdb_api.insert_one('db', '4col', '{ "_id": 2}');

-- (1.2)  multiple-insert to 4 column collection
SELECT documentdb_api.insert('db', '{"insert":"4col", "documents":[ { "_id" : 3}, { "_id" : 4}, { "_id" : 5}]}');

-- (1.3)  update to 4 column collection
SELECT documentdb_api.update('db', '{"update":"4col", "updates":[{"q":{"_id":{"$eq":1}},"u":[{"$set":{"a" : 1} }]}]}');

-- (1.4)  aggregate to 4 column collection
SELECT * FROM aggregate_cursor_first_page('db', '{ "aggregate": "4col", "pipeline": [ {"$match" : {}} ] , "cursor": { "batchSize": 10 } }', 4294967294);

-- (1.5)  aggregate to 4 column collection
SELECT * FROM aggregate_cursor_first_page('db', '{ "aggregate": "4col",  "pipeline": [  {"$project" : {"a" : "GUC IS ENABLED"}},{"$merge" : { "into": "4col",  "whenMatched" : "replace" , "whenNotMatched" : "insert" }} ] , "cursor": { "batchSize": 1 } }', 4294967294);
SELECT * FROM aggregate_cursor_first_page('db', '{ "aggregate": "4col", "pipeline": [ {"$match" : {}} ] , "cursor": { "batchSize": 10 } }', 4294967294);


-- [2] let's test 3 column table after enabling GUC
SELECT documentdb_api.create_collection('db', '3col');

\d documentdb_data.documents_293001

-- (2.1)  insert to 3 column collection
SELECT documentdb_api.insert_one('db', '3col', '{ "_id": 2}');

-- (2.2)  multiple-insert to 3 column collection
SELECT documentdb_api.insert('db', '{"insert":"3col", "documents":[ { "_id" : 3}, { "_id" : 4}, { "_id" : 5}]}');

-- (2.3)  update to 3 column collection
SELECT documentdb_api.update('db', '{"update":"3col", "updates":[{"q":{"_id":{"$eq":1}},"u":[{"$set":{"a" : 1} }]}]}');

-- (2.4)  aggregate to 3 column collection
SELECT * FROM aggregate_cursor_first_page('db', '{ "aggregate": "3col",  "pipeline": [  {"$project" : {"a" : "GUC IS ENABLED"}},{"$merge" : { "into": "3col",  "whenMatched" : "replace" , "whenNotMatched" : "insert" }} ] , "cursor": { "batchSize": 1 } }', 4294967294);
SELECT * FROM aggregate_cursor_first_page('db', '{ "aggregate": "3col", "pipeline": [ {"$match" : {}} ] , "cursor": { "batchSize": 10 } }', 4294967294);

--3. let's disable GUC
SET documentdb.enableDataTableWithoutCreationTime to off;

-- (3.1)  insert to 3 column collection
SELECT documentdb_api.insert_one('db', '3col', '{ "_id": 200}');

-- (3.2)  multiple-insert to 3 column collection
SELECT documentdb_api.insert('db', '{"insert":"3col", "documents":[ { "_id" : 300}, { "_id" : 400}, { "_id" : 500}]}');

-- (3.3)  update to 3 column collection
SELECT documentdb_api.update('db', '{"update":"3col", "updates":[{"q":{"_id":{"$eq":1}},"u":[{"$set":{"a" : 1} }]}]}');

-- (3.4)  aggregate to 3 column collection
SELECT * FROM aggregate_cursor_first_page('db', '{ "aggregate": "3col", "pipeline": [ {"$match" : {}} ] , "cursor": { "batchSize": 1 } }', 4294967294);
-- (3.5)  $merge to 4 column collection
SELECT * FROM aggregate_cursor_first_page('db', '{ "aggregate": "3col",  "pipeline": [  {"$project" : {"a" : "GUC IS DISBALE"}},{"$merge" : { "into": "3col",  "whenMatched" : "replace" , "whenNotMatched" : "insert" }} ] , "cursor": { "batchSize": 1 } }', 4294967294);
SELECT * FROM aggregate_cursor_first_page('db', '{ "aggregate": "3col", "pipeline": [ {"$match" : {}} ] , "cursor": { "batchSize": 10 } }', 4294967294);


-- lookup test
-- create 4 column table for lookup
SELECT documentdb_api.insert_one('db','pipelinefroms',' {"_id": 1, "name": "American Steak House", "food": ["filet", "sirloin"], "quantity": 100 , "beverages": ["beer", "wine"]}', NULL);
SELECT documentdb_api.insert_one('db','pipelinefroms','{ "_id": 2, "name": "Honest John Pizza", "food": ["cheese pizza", "pepperoni pizza"], "quantity": 120, "beverages": ["soda"]}', NULL);
\d documentdb_data.documents_293002
-- now create table with 3 column
SET documentdb.enableDataTableWithoutCreationTime to on;
SELECT documentdb_api.insert_one('db','pipelinetos','{ "_id": 1, "item": "filet", "restaurant_name": "American Steak House"}', NULL);
SELECT documentdb_api.insert_one('db','pipelinetos','{ "_id": 2, "item": "cheese pizza", "restaurant_name": "Honest John Pizza", "drink": "lemonade"}', NULL);
SELECT documentdb_api.insert_one('db','pipelinetos','{ "_id": 3, "item": "cheese pizza", "restaurant_name": "Honest John Pizza", "drink": "soda"}', NULL);
\d documentdb_data.documents_293003

SELECT document from bson_aggregation_pipeline('db', 
  '{ "aggregate": "pipelinetos", "pipeline": [ { "$lookup": { "from": "pipelinefroms", "pipeline": [ { "$match": { "quantity": { "$gt": 110 } }}], "as": "matched_docs", "localField": "restaurant_name", "foreignField": "name" }} ], "cursor": {} }');

