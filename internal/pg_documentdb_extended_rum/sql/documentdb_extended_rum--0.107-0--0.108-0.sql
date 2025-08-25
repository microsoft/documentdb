
CREATE SCHEMA documentdb_extended_rum_jsonb_ops;
CREATE SCHEMA documentdb_extended_rum_jsonb_ops_internal;
GRANT USAGE ON SCHEMA documentdb_extended_rum_jsonb_ops TO public;
GRANT USAGE ON SCHEMA documentdb_extended_rum_jsonb_ops_internal to public;

#include "udfs/jsonb/jsonbpath_functions--0.108-0.sql"
#include "schema/jsonb/jsonbpath_operators--0.108-0.sql"
#include "udfs/jsonb/jsonbpath_operator_class_functions--0.108-0.sql"
#include "schema/jsonb/jsonbpath_operator_class--0.108-0.sql"