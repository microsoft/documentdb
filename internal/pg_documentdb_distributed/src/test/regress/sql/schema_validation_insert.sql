SET search_path TO documentdb_core,documentdb_api,documentdb_api_catalog,documentdb_api_internal;
SET citus.next_shard_id TO 17771000;
SET documentdb.next_collection_id TO 177710;
SET documentdb.next_collection_index_id TO 177710;
set documentdb.enableSchemaValidation = true;

--------------------------------------Need $jsonSchema--------------------------------------
SELECT documentdb_api.create_collection_view('schema_validation_insertion', '{ "create": "col", "validator": {"$jsonSchema": {"bsonType": "object", "properties": {"a": {"bsonType": "int"}}}}, "validationLevel": "strict", "validationAction": "error"}');

SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col", "documents":[{"_id":"1", "a":1}]}');
-- required not supported yet, so this should be inserted
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col", "documents":[{"_id":"2", "b":1}]}');
-- type mismatch
SELECT documentdb_api.insert('schema_validation_insertion','{"insert":"col", "documents":[{"_id":"3", "a":"hello"}]}');
-- batch insert
SELECT documentdb_api.insert('schema_validation_insertion','{"insert":"col", "documents":[{"_id":"4", "a":2},{"_id":"5", "a":3}, {"_id":"6", "a":"tt"}]}');
-- 0 documents should be inserted
SELECT shard_key_value, object_id, document from documentdb_api.collection('schema_validation_insertion','col') ORDER BY shard_key_value, object_id;
-- set validationAction to warn
SELECT documentdb_api.coll_mod('schema_validation_insertion', 'col', '{"collMod":"col", "validationAction": "warn"}');
SELECT documentdb_api.insert('schema_validation_insertion','{"insert":"col", "documents":[{"_id":"7", "a":"hello"}]}');
-- 1 document should be inserted
SELECT shard_key_value, object_id, document from documentdb_api.collection('schema_validation_insertion','col') ORDER BY shard_key_value, object_id;




---------------------------------------------Need top level operator-----------------------------------------------------
-- $expr
SELECT documentdb_api.create_collection_view('schema_validation_insertion', '{ "create": "col1", "validator": { "$expr": {"$eq": [ "$a", "$b" ] } } }');
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col1", "documents":[{"_id":"1", "a":1, "b":1, "c":1}]}');
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col1", "documents":[{"_id":"2", "a":3, "b":1, "c":2}]}');

-- $and
SELECT documentdb_api.create_collection_view('schema_validation_insertion', '{ "create": "col2", "validator": { "$and": [ { "a": { "$gt": 2 } }, {"$jsonSchema": {"bsonType": "object", "properties": {"a": {"bsonType": "int", "maximum":5}}}} ] } }');
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col2", "documents":[{"_id":"1", "a":4}]}');
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col2", "documents":[{"_id":"2", "a":1}]}');
-- expect to throw error as 6 > 5 (maximum)
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col2", "documents":[{"_id":"3", "a":6}]}');
set documentdb.enableBypassDocumentValidation = true;
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col2", "documents":[{"_id":"2", "a":1}], "bypassDocumentValidation": true}');

---------------------------------------------simple case-----------------------------------------------------
-- field 
SELECT documentdb_api.create_collection_view('schema_validation_insertion', '{ "create": "col3", "validator": {"a":{"$type":"int"}}}');
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col3", "documents":[{"_id":"1", "a":1}]}');
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col3", "documents":[{"_id":"2", "a":"hello"}]}');

--$merge
--todo - need to check
SELECT documentdb_api.insert('schema_validation_insertion','{"insert":"col_", "documents":[{"_id":"1001","a":"world"}]}');
SELECT documentdb_api.insert('schema_validation_insertion','{"insert":"col_", "documents":[{"_id":"1002","a":2}]}');
SELECT * FROM aggregate_cursor_first_page('schema_validation_insertion', '{ "aggregate": "col_", "pipeline": [ { "$match": { "a": { "$type": "string" }}}, {"$merge" : { "into": "col3" }} ], "cursor": { "batchSize": 1 } }', 4294967294);
SELECT * FROM aggregate_cursor_first_page('schema_validation_insertion', '{ "aggregate": "col_", "pipeline": [ { "$match": { "a": { "$type": "int" }}}, {"$merge" : { "into": "col3" }} ], "cursor": { "batchSize": 1 } }', 4294967294);
SELECT shard_key_value, object_id, document from documentdb_api.collection('schema_validation_insertion','col3') ORDER BY shard_key_value, object_id;

-- sharded collection test
SELECT documentdb_api.shard_collection('schema_validation_insertion', 'col3', '{ "a": "hashed" }', false);
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col3", "documents":[{"_id":"1", "a":"hello"}]}');
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col3", "documents":[{"_id":"2", "a":5}]}');
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col3", "documents":[{"_id":"3", "a":2}, {"_id":"4", "a":3}, {"_id":"5", "a":4}, {"_id":"6", "a":"string"}]}');
-- 5 documents should be inserted
SELECT shard_key_value, object_id, document from documentdb_api.collection('schema_validation_insertion','col3') ORDER BY shard_key_value, object_id;
-- set validationAction to warn
SELECT documentdb_api.coll_mod('schema_validation_insertion', 'col3', '{"collMod":"col3", "validationAction": "warn"}');
SELECT documentdb_api.insert('schema_validation_insertion','{"insert":"col3", "documents":[{"_id":"7", "a":"hello"}]}');
-- 6 document should be inserted
SELECT shard_key_value, object_id, document from documentdb_api.collection('schema_validation_insertion','col3') ORDER BY shard_key_value, object_id;


---------------------------------------------update-----------------------------------------------------
-- sharded collection test
-- will succeed as validationAction is warn
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col3", "updates":[{"q":{"a":1},"u":{"$set":{"a":"one"}}}]}');
-- set validation action to error
SELECT documentdb_api.coll_mod('schema_validation_insertion', 'col3', '{"collMod":"col3", "validationAction": "error"}');
-- should throw error
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col3", "updates":[{"q":{"a":4},"u":{"$set":{"a":"four"}}}]}');
-- should succeed
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col3", "updates":[{"q":{"a":3},"u":{"$set":{"a":300}}}]}');
-- upsert succeeded
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col3", "updates":[{"q":{"a":"abc"},"u":{"$set":{"_id":500, "a":500}}, "upsert":true}]}');
-- upsert failed
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col3", "updates":[{"q":{"a":"abc"},"u":{"$set":{"a":"abcd"}}, "upsert":true}]}');
-- should succeed with bypassDocumentValidation
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col3", "updates":[{"q":{"a":4},"u":{"$set":{"a":"four"}}}], "bypassDocumentValidation": true}');

-- multiple updates
-- throw error as multi update is not allowed on sharded collection
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col3", "updates":[{"q":{"a":2},"u":{"$set":{"a":200}}, "multi":true} ]}');
SELECT shard_key_value, object_id, document from documentdb_api.collection('schema_validation_insertion','col3');

-- will throw error as validationLevel is strict
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col3", "updates":[{"q":{"a":"four"},"u":{"$set":{"a":"fourty"}}} ]}');
-- moderate case
SELECT documentdb_api.coll_mod('schema_validation_insertion', 'col3', '{"collMod":"col3", "validationLevel": "moderate"}');
-- will succeed as validationLevel is moderate
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col3", "updates":[{"q":{"a":"four"},"u":{"$set":{"a":"fourty"}}} ]}');
SELECT shard_key_value, object_id, document from documentdb_api.collection('schema_validation_insertion','col3');
-- batch update
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col3", "updates":[{"q":{"a":500},"u":{"$set":{"a":5000}}}, {"q":{"a":"four"},"u":{"$set":{"a":"fourty"}}}, {"q":{"a":6},"u":{"$set":{"a":600, "_id":600}}, "upsert": true}, {"q":{"a":"string"},"u":{"$set":{"a":"str"}}, "upsert":true} ]}');
 
--unsharded collection test
SELECT documentdb_api.create_collection_view('schema_validation_insertion', '{ "create": "col4", "validator": {"a":{"$type":"int"}}, "validationLevel": "strict", "validationAction": "warn"}');
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col4", "documents":[{"_id":"1", "a":1}, {"_id":"2", "a":2}, {"_id":"3", "a":3}]}');
SELECT documentdb_api.insert('schema_validation_insertion', '{"insert":"col4", "documents":[{"_id":"4", "a":"hello"}]}');
-- will succeed as validationAction is warn
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col4", "updates":[{"q":{"a":1},"u":{"$set":{"a":"one"}}}]}');
SELECT documentdb_api.coll_mod('schema_validation_insertion', 'col4', '{"collMod":"col4", "validationAction": "error"}');
-- should throw error
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col4", "updates":[{"q":{"a":2},"u":{"$set":{"a":"one"}}}]}');
-- should succeed
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col4", "updates":[{"q":{"a":3},"u":{"$set":{"a":300}}}]}');
-- upsert succeeded
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col4", "updates":[{"q":{"a":"abc"},"u":{"$set":{"_id":500, "a":500}}, "upsert":true}]}');
-- upsert failed
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col4", "updates":[{"q":{"a":"abc"},"u":{"$set":{"a":"abcd"}}, "upsert":true}]}');
-- should succeed with bypassDocumentValidation
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col4", "updates":[{"q":{"a":2},"u":{"$set":{"a":"one"}}}], "bypassDocumentValidation": true}');

-- multiple updates
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col4", "updates":[{"q":{"a":"one"},"u":{"$set":{"a":200}}, "multi":true} ]}');
-- will throw error 
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col4", "updates":[{"q":{"a":300},"u":{"$set":{"a":"th"}}, "multi":true} ]}');

-- will throw error as validationLevel is strict
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col4", "updates":[{"q":{"a":"hello"},"u":{"$set":{"a":"world"}}} ]}');
-- moderate case
SELECT documentdb_api.coll_mod('schema_validation_insertion', 'col4', '{"collMod":"col4", "validationLevel": "moderate"}');
-- will succeed as validationLevel is moderate
SELECT documentdb_api.update('schema_validation_insertion', '{"update":"col4", "updates":[{"q":{"a":"hello"},"u":{"$set":{"a":"ten"}}} ]}');

SELECT shard_key_value, object_id, document from documentdb_api.collection('schema_validation_insertion','col4');
