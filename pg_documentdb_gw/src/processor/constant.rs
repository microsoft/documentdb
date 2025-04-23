/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/processor/constant.rs
 *
 *-------------------------------------------------------------------------
 */

use std::{
    sync::Arc,
    time::{SystemTime, UNIX_EPOCH},
};

use bson::{rawdoc, RawDocumentBuf};

use crate::{
    configuration::DynamicConfiguration,
    context::ConnectionContext,
    error::{DocumentDBError, ErrorCode, Result},
    protocol::{self, OK_SUCCEEDED},
    requests::{Request, RequestInfo},
    responses::{RawResponse, Response},
};

pub fn ok_response() -> Response {
    Response::Raw(RawResponse(rawdoc! {
        "ok": OK_SUCCEEDED
    }))
}

pub async fn process_build_info(
    dynamic_config: &Arc<dyn DynamicConfiguration>,
) -> Result<Response> {
    let version = dynamic_config.server_version().await;
    Ok(Response::Raw(RawResponse(rawdoc! {
        "version": version.as_str(),
        "versionArray": version.as_bson_array(),
        "bits": 64,
        "maxBsonObjectSize": protocol::MAX_BSON_OBJECT_SIZE,
        "ok":OK_SUCCEEDED,
    })))
}

pub fn process_get_cmd_line_opts() -> Result<Response> {
    Ok(Response::Raw(RawResponse(rawdoc! {
        "argv": [],
        "ok":OK_SUCCEEDED,
    })))
}

pub fn process_is_db_grid(context: &ConnectionContext) -> Result<Response> {
    Ok(Response::Raw(RawResponse(rawdoc! {
        "isdbgrid":1.0,
        "hostname":context.service_context.setup_configuration().node_host_name(),
        "ok":OK_SUCCEEDED,
    })))
}

pub fn process_get_rw_concern(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    _: &ConnectionContext,
) -> Result<Response> {
    request.extract_fields(|k, _| match k {
        "getDefaultRWConcern" | "inMemory" | "comment" | "lsid" | "$db" => Ok(()),
        other => Err(DocumentDBError::documentdb_error(
            ErrorCode::UnknownBsonField,
            format!("Unknown field for getDefaultRWConcern: {}", other),
        )),
    })?;

    if request_info.db()? != "admin" {
        return Err(DocumentDBError::documentdb_error(
            ErrorCode::Unauthorized,
            "getDefaultRWConcern may only be run against the admin database.".to_string(),
        ));
    }

    Ok(Response::Raw(RawResponse(rawdoc! {
        "defaultReadConcern": {
            "level":"majority",
        },
        "defaultWriteConcern": {
            "w": "majority",
            "wtimeout": 0,
        },
        "defaultReadConcernSource": "implicit",
        "defaultWriteConcernSource": "implicit",
        "ok":OK_SUCCEEDED,
    })))
}

pub fn process_get_log() -> Result<Response> {
    Ok(Response::Raw(RawResponse(rawdoc! {
        "log":[],
        "totalLinesWritten":0,
        "ok":OK_SUCCEEDED,
    })))
}

pub fn process_connection_status() -> Result<Response> {
    Ok(Response::Raw(RawResponse(rawdoc! {
        "authInfo": {
            "authenticatedUsers": [],
            "authenticatedUserRoles": [],
            "authenticatedUserPrivileges": [],
        },
        "ok":OK_SUCCEEDED,
    })))
}

fn local_time() -> Result<u32> {
    u32::try_from(
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|_| {
                DocumentDBError::internal_error("Failed to get the current time".to_string())
            })?
            .as_secs(),
    )
    .map_err(|_| DocumentDBError::internal_error("Current time exceeded an u32".to_string()))
}

pub fn process_host_info() -> Result<Response> {
    Ok(Response::Raw(RawResponse(rawdoc! {
        "system": {
            "currentTime": bson::Timestamp{ time: local_time()?, increment: 0},
            "memSizeMB": 0,
        },
        "os": {
            "name":"",
            "type":"",
        },
        "extra": {
            "cpuFrequencyMHz": 0,
        },
        "ok": OK_SUCCEEDED,
    })))
}

pub fn process_prepare_transaction() -> Result<Response> {
    Ok(Response::Raw(RawResponse(rawdoc! {
        "prepareTimestamp":  bson::Timestamp{ time: local_time()?, increment: 0 },
        "ok": OK_SUCCEEDED,
    })))
}

pub fn process_whats_my_uri() -> Result<Response> {
    Ok(Response::Raw(RawResponse(rawdoc! {
        "ok": OK_SUCCEEDED,
    })))
}

struct CommandInfo {
    command_name: &'static str,
    admin_only: bool,
    help: &'static str,
    secondary_ok: bool,
    requires_auth: bool,
    secondary_override_ok: Option<bool>,
}

static SUPPORTED_COMMANDS : [CommandInfo; 55] = [
    CommandInfo {
		command_name: "abortTransaction",
		admin_only: true,
		help: "Aborts a transaction",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "aggregate",
		admin_only: false,
		help: "Runs the sharded aggregation command. See http://dochub.mongodb.org/core/aggregation for more details.",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: Some(false),
	},
    CommandInfo {
		command_name: "authenticate",
		admin_only: false,
		help: "no help defined",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "buildInfo",
		admin_only: false,
		help: "get version #, etc.\n{ buildinfo:1 }",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "collMod",
		admin_only: false,
		help: "Sets collection options.\nExample: { collMod: 'foo', viewOn: 'bar'} Example: { collMod: 'foo', index: {keyPattern: {a: 1}, expireAfterSeconds: 600} Example: { collMod: 'foo', index: {name: 'bar', expireAfterSeconds: 600} }\n",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "collStats",
		admin_only: false,
		help: "{ collStats:\"blog.posts\" , scale : 1 } scale divides sizes e.g. for KB use 1024\n    avgObjSize - in bytes",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "commitTransaction",
		admin_only: true,
		help: "Commits a transaction",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "connectionStatus",
		admin_only: false,
		help: "Returns connection-specific information such as logged-in users and their roles",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "count",
		admin_only: false,
		help: "count objects in collection",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: Some(false),
	},
    CommandInfo {
		command_name: "create",
		admin_only: false,
		help: "explicitly creates a collection or view\n{\n  create: <string: collection or view name> [,\n  capped: <bool: capped collection>,\n  autoIndexId: <bool: automatic creation of _id index>,\n  idIndex: <document: _id index specification>,\n  size: <int: size in bytes of the capped collection>,\n  max: <int: max number of documents in the capped collection>,\n  storageEngine: <document: storage engine configuration>,\n  validator: <document: validation rules>,\n  validationLevel: <string: validation level>,\n  validationAction: <string: validation action>,\n  indexOptionDefaults: <document: default configuration for indexes>,\n  viewOn: <string: name of source collection or view>,\n  pipeline: <array<object>: aggregation pipeline stage>,\n  collation: <document: default collation for the collection or view>,\n  writeConcern: <document: write concern expression for the operation>]\n}",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "createIndexes",
		admin_only: false,
		help: "no help defined",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "currentOp",
		admin_only: true,
		help: "no help defined",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "dbStats",
		admin_only: false,
		help: "{ dbStats : 1 , scale : 1 } scale divides sizes e.g. for KB use 1024\n    avgObjSize - in bytes",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "delete",
		admin_only: false,
		help: "delete documents",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "distinct",
		admin_only: false,
		help: "{ distinct : 'collection name' , key : 'a.b' , query : {} }",
		secondary_ok: false,
		requires_auth: true,
		secondary_override_ok: Some(false),
	},
    CommandInfo {
		command_name: "drop",
		admin_only: false,
		help: "drop a collection\n{drop : <collectionName>}",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "dropDatabase",
		admin_only: false,
		help: "drop (delete) this database",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "dropIndexes",
		admin_only: false,
		help: "drop indexes for a collection",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "enableSharding",
		admin_only: true,
		help: "Enable sharding for a database. Optionally allows the caller to specify the shard to be used as primary.(Use 'shardcollection' command afterwards.)\n  { enableSharding : \"<dbname>\", primaryShard:  \"<shard>\"}\n",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "endSessions",
		admin_only: false,
		help: "end a set of logical sessions",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "explain",
		admin_only: false,
		help: "explain database reads and writes",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: Some(false),
	},
    CommandInfo {
		command_name: "find",
		admin_only: false,
		help: "query for documents",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: Some(false),
	},
    CommandInfo {
		command_name: "findAndModify",
		admin_only: false,
		help: "{ findAndModify: \"collection\", query: {processed:false}, update: {$set: {processed:true}}, new: true}\n{ findAndModify: \"collection\", query: {processed:false}, remove: true, sort: {priority:-1}}\nEither update or remove is required, all other fields have default values.\nOutput is in the \"value\" field\n",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "getCmdLineOpts",
		admin_only: true,
		help: "get argv",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "getDefaultRWConcern",
		admin_only: true,
		help: "get cluster-wide read and write concern",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "getLastError",
		admin_only: false,
		help: "return error status of the last operation on this connection\n",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "getLog",
		admin_only: true,
		help: "{ getLog : '*' }  OR { getLog : 'global' }",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "getMore",
		admin_only: false,
		help: "retrieve more results from an existing cursor",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "getParameter",
		admin_only: true,
		help: "get administrative option(s) example: { getParameter:1, transactionLifetimeLimitSeconds:1 } pass a document as the value for getParameter to request options",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "getnonce",
		admin_only: false,
		help: "internal",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "hello",
		admin_only: false,
		help: "Check if this server is primary for a replica set\n{ hello : 1 }",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "hostInfo",
		admin_only: false,
		help: "returns information about the daemon's host",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "insert",
		admin_only: false,
		help: "insert documents",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "isMaster",
		admin_only: false,
		help: "Check if this server is primary for a replica set\n{ isMaster : 1 }",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "isdbgrid",
		admin_only: false,
		help: "check if the server instance is mongos\n{ isdbgrid: 1 }",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
	},
    CommandInfo {
		command_name: "killCursors",
		admin_only: false,
		help: "Kill a list of cursor ids",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "killOp",
		admin_only: true,
		help: "no help defined",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "killSessions",
		admin_only: false,
		help: "kill a logical session and its operations",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "listCollections",
		admin_only: false,
		help: "list collections for this db",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: Some(false),
    },
    CommandInfo {
		command_name: "listCommands",
		admin_only: false,
		help: "get a list of all db commands",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "listDatabases",
		admin_only: true,
		help: "{ listDatabases:1, [filter: <filterObject>] [, nameOnly: true ] }\nlist databases on this server",
		secondary_ok: false,
		requires_auth: true,
		secondary_override_ok: Some(false),
    },
    CommandInfo {
		command_name: "listIndexes",
		admin_only: false,
		help: "list indexes for a collection",
		secondary_ok: false,
		requires_auth: true,
		secondary_override_ok: Some(false),
    },
    CommandInfo {
		command_name: "logout",
		admin_only: false,
		help: "de-authenticdbate",
		secondary_ok: false,
		requires_auth: true,
		secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "ping",
		admin_only: false,
		help: "a way to check that the server is alive. responds immediately even if server is in a db lock.",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "reIndex",
		admin_only: false,
		help: "re-index a collection",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "renameCollection",
		admin_only: true,
		help: "example: { renameCollection: foo.a, to: bar.b }",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "reshardCollection",
		admin_only: true,
		help: "Reshard an already sharded collection on a new shard key.",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "saslContinue",
		admin_only: false,
		help: "Subsequent steps in a SASL authentication conversation.",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "saslStart",
		admin_only: false,
		help: "First step in a SASL authentication conversation.",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "serverStatus",
		admin_only: false,
		help: "returns lots of administrative server statistics",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "shardCollection",
		admin_only: true,
		help: "Shard a collection. Requires key. Optional unique. Sharding must already be enabled for the database.\n   { enablesharding : \"<dbname>\" }\n",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "startSession",
		admin_only: false,
		help: "start a logical session",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "update",
		admin_only: false,
		help: "update documents",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "validate",
		admin_only: false,
		help: "Validate contents of a namespace by scanning its data structures for correctness.\n This is a slow operation.\n\tAdd {full: true} option to do a more thorough check.\n\tAdd {background: true} to validate in the background.\n\tAdd {repair: true} to run repair mode.\nCannot specify both {full: true, background: true}.",
		secondary_ok: false,
		requires_auth: true,
        secondary_override_ok: None,
    },
    CommandInfo {
		command_name: "whatsmyuri",
		admin_only: false,
		help: "{whatsmyuri:1}",
		secondary_ok: false,
		requires_auth: false,
        secondary_override_ok: None,
    }
];

pub fn list_commands() -> Result<Response> {
    let mut commands_doc = RawDocumentBuf::new();
    for command in &SUPPORTED_COMMANDS {
        let mut doc = rawdoc! {
            "adminOnly": command.admin_only,
            "apiVersions": [],
            "deprecatedApiVersions": [],
            "help": command.help,
            "secondaryOk": command.secondary_ok,
            "requiresAuth": command.requires_auth,
        };
        if let Some(secondary_override) = command.secondary_override_ok {
            doc.append("secondaryOverrideOk", secondary_override)
        }
        commands_doc.append(command.command_name, doc);
    }

    Ok(Response::Raw(RawResponse(rawdoc! {
        "commands": commands_doc,
        "ok": OK_SUCCEEDED,
    })))
}
