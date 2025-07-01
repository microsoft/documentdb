CREATE FUNCTION documentdb_rumhandler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

/*
 * RUM access method
 */

CREATE ACCESS METHOD documentdb_rum TYPE INDEX HANDLER documentdb_rumhandler;
