---

uid: schema_changes_for_rbac.md
title: Schema changes to facilitate RBAC in DocumentDB

---

# Changing Default Permissions of Functions in DocumentDB

Engineering Owner: deepakpe

## Overview

Today the execution privileges on all the functions in our schemas are set to PUBLIC.
In light of the upcoming feature to support RoleBasedAccessControl we want to remove the ability to execute functions from public from all our schemas and grant access as needed to specific built-in roles.

## Design

Today we have 4 built-in roles in documentdb.

1. documentdb role : This role is a superuser. As a result it can execute all functions in all the schemas without us having to grant it any specific privileges.
2. documentdb_admin_role : This role can perform read write operations across the entire cluster. In addition it can also perform admin operations such as sharding. This is the role users of documentdb_local get by default.
3. documentdb_readonly_role: This role has read access across the entire cluster.
4. documentdb_bg_worker_role: This is role is used to run background processes.

The functions in documentdb are spread across three schemas.

1. documentdb_api
2. documentdb_api_internal
3. documentdb_api_catalog

### documentdb_api

The table below lists the current functions and procedures in documentdb_api and which built-in roles are allowed to execute them. 
We will not be changing the documentdb_api schema itself.
Instead we will create a new schema documentdb_api_v2 where the ability to execute functions is revoked from PUBLIC and the ability to execute specific functions is granted only to specific built-in roles.

| Schema         | Name                               | Type | Built-in roles that have execute privileges                           |
|----------------|------------------------------------|------|-----------------------------------------------------------------------|
| documentdb_api | create_user                        | func | documentdb                                                            |
| documentdb_api | drop_user                          | func | documentdb                                                            |
| documentdb_api | update_user                        | func | documentdb (but every user can use this to change their own password) |
| documentdb_api | binary_extended_version            | func | documentdb_admin_role                                                 |
| documentdb_api | binary_version                     | func | documentdb_admin_role                                                 |
| documentdb_api | coll_mod                           | func | documentdb_admin_role                                                 |
| documentdb_api | collection                         | func | documentdb_admin_role                                                 |
| documentdb_api | compact                            | func | documentdb_admin_role                                                 |
| documentdb_api | create_collection                  | func | documentdb_admin_role                                                 |
| documentdb_api | create_collection_view             | func | documentdb_admin_role                                                 |
| documentdb_api | create_indexes_background          | func | documentdb_admin_role                                                 |
| documentdb_api | delete                             | func | documentdb_admin_role                                                 |
| documentdb_api | drop_collection                    | func | documentdb_admin_role                                                 |
| documentdb_api | drop_database                      | func | documentdb_admin_role                                                 |
| documentdb_api | drop_indexes                       | proc | documentdb_admin_role                                                 |
| documentdb_api | insert                             | func | documentdb_admin_role                                                 |
| documentdb_api | insert_bulk                        | proc | documentdb_admin_role                                                 |
| documentdb_api | insert_one                         | func | documentdb_admin_role                                                 |
| documentdb_api | rename_collection                  | func | documentdb_admin_role                                                 |
| documentdb_api | reshard_collection                 | func | documentdb_admin_role                                                 |
| documentdb_api | shard_collection                   | func | documentdb_admin_role                                                 |
| documentdb_api | shard_collection                   | func | documentdb_admin_role                                                 |
| documentdb_api | unshard_collection                 | func | documentdb_admin_role                                                 |
| documentdb_api | update                             | func | documentdb_admin_role                                                 |
| documentdb_api | update_bulk                        | proc | documentdb_admin_role                                                 |
| documentdb_api | find_and_modify                    | func | documentdb_admin_role                                                 |
| documentdb_api | aggregate_cursor_first_page        | func | documentdb_admin_role, documentdb_readonly_role                       |
| documentdb_api | coll_stats                         | func | documentdb_admin_role, documentdb_readonly_role                       |
| documentdb_api | connection_status                  | func | documentdb_admin_role, documentdb_readonly_role                       |
| documentdb_api | count_query                        | func | documentdb_admin_role, documentdb_readonly_role                       |
| documentdb_api | current_op_command                 | func | documentdb_admin_role, documentdb_readonly_role                       |
| documentdb_api | cursor_get_more                    | func | documentdb_admin_role, documentdb_readonly_role                       |
| documentdb_api | db_stats                           | func | documentdb_admin_role, documentdb_readonly_role                       |
| documentdb_api | distinct_query                     | func | documentdb_admin_role, documentdb_readonly_role                       |
| documentdb_api | find_cursor_first_page             | func | documentdb_admin_role, documentdb_readonly_role                       |
| documentdb_api | list_collections_cursor_first_page | func | documentdb_admin_role, documentdb_readonly_role                       |
| documentdb_api | list_databases                     | func | documentdb_admin_role, documentdb_readonly_role                       |
| documentdb_api | list_indexes_cursor_first_page     | func | documentdb_admin_role, documentdb_readonly_role                       |
| documentdb_api | users_info                         | func | documentdb_admin_role, documentdb_readonly_role                       |
| documentdb_api | validate                           | func | documentdb_admin_role, documentdb_readonly_role                       |

### documentdb_api_catalog

All the functions and procedures in documentdb_api_catalog are read only. As a result both documentdb_admin_role and documentdb_readonly_role have execute privileges on all functions and procs in documentdb_api_catalog. As a result we will not be making any changes to this schema.

### documentdb_api_internal

As part of this work we want to divide the documentdb_api_internal schema into four seperate schemas namely

1. documentdb_api_internal_readonly
2. documentdb_api_internal_readwrite
3. documentdb_api_internal_admin
4. documentdb_api_internal_bgworker

This would make the functions more maintainable going ahead, makes default permissioning easier and also help with migration by giving us the ability to switch back to the old schemas in case we see any problems.
When adding a new function we should add it to the lowest permissible schema that can execute the function. 
For instance, if we were adding a new read function, the lowest level of permissions one would need to use the function would be read privileges. So we add it to the schema documentdb_api_internal_readonly.

The table below lists the functions in documentdb_api_internal, the new schema it should belong to and the built-in roles that have execute privileges on them

| Schema                  | Name                                         | Type   | New Schema                        | Built-in roles that have execute privileges     |
|-------------------------|----------------------------------------------|--------|-----------------------------------|-------------------------------------------------|
| documentdb_api_internal | cursor_directory_cleanup                     | func   | documentdb_api_internal_bgworker  | documentdb_bg_worker_role                       |
| documentdb_api_internal | get_bloat_stats_worker                       | func   | documentdb_api_internal_bgworker  | documentdb_bg_worker_role                       |
| documentdb_api_internal | schedule_background_index_build_jobs         | func   | documentdb_api_internal_bgworker  | documentdb_bg_worker_role                       |
| documentdb_api_internal | generate_unique_shard_document               | func   | documentdb_api_internal_admin     | documentdb_admin_role                           |
| documentdb_api_internal | invalidate_collection_cache                  | func   | documentdb_api_internal_admin     | documentdb_admin_role                           |
| documentdb_api_internal | record_id_index                              | func   | documentdb_api_internal_admin     | documentdb_admin_role                           |
| documentdb_api_internal | delete_worker                                | func   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | empty_data_table                             | func   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | apply_extension_data_table_upgrade           | func   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | build_index_background                       | proc   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | build_index_concurrently                     | proc   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | collection_update_trigger                    | func   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | create_builtin_id_index                      | func   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | create_indexes_background_internal           | func   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | create_indexes_non_concurrently              | func   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | delete_expired_rows                          | proc   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | delete_expired_rows_background               | proc   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | delete_one                                   | func   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | insert_one                                   | func   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | insert_worker                                | func   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | update_bson_document                         | func   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | update_one                                   | func   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | update_worker                                | func   | documentdb_api_internal_readwrite | documentdb_admin_role                           |
| documentdb_api_internal | get_shard_key_value                          | func   | documentdb_api_internal_admin     | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | check_build_index_status                     | func   | documentdb_api_internal_readwrite | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | check_build_index_status_internal            | func   | documentdb_api_internal_readwrite | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | index_build_is_in_progress                   | func   | documentdb_api_internal_readwrite | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | index_spec_as_bson                           | func   | documentdb_api_internal_readwrite | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | index_spec_options_are_equivalent            | func   | documentdb_api_internal_readwrite | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | index_stats_aggregation                      | func   | documentdb_api_internal_readwrite | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | index_stats_worker                           | func   | documentdb_api_internal_readwrite | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | delete_cursors                               | func   | documentdb_api_internal_admin     | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_unique_shard_consistent             | func   | documentdb_api_internal_admin     | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_unique_shard_extract_query          | func   | documentdb_api_internal_admin     | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_unique_shard_extract_value          | func   | documentdb_api_internal_admin     | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_unique_shard_pre_consistent         | func   | documentdb_api_internal_admin     | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | schema_validation_against_update             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | scram_sha256_get_salt_and_iterations         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | cursor_state                                 | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | db_stats_worker                              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | cursor_state                                 | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | current_cursor_state                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | current_op_aggregation                       | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | current_op_worker                            | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | command_feature_counter_stats                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | coll_stats_aggregation                       | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | coll_stats_worker                            | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | ensure_valid_db_coll                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | documentdb_core_bson_to_bson                 | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | documentdb_get_next_collection_id            | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | documentdb_get_next_collection_index_id      | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | aggregation_support                          | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | authenticate_with_scram_sha256               | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_add_to_set                              | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_add_to_set_final                        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_add_to_set_transition                   | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_array_agg_minvtransition                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_const_fill                              | window | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_covariance_pop_final                    | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_covariance_pop_samp_combine             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_covariance_pop_samp_invtransition       | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_covariance_pop_samp_transition          | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_covariance_samp_final                   | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dense_rank                              | window | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_densify_full                            | window | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_densify_partition                       | window | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_densify_range                           | window | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_derivative_transition                   | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_distinct_array_agg_final                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_distinct_array_agg_transition           | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_document_add_score_field                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_document_number                         | window | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_add_fields                       | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_add_fields                       | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_bucket_auto                      | window | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_eq                               | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_expr                             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_extract_merge_filter             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_fullscan                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_gt                               | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_gte                              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_inverse_match                    | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_lookup_expression_eval_merge     | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_lookup_extract_filter_array      | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_lookup_extract_filter_expression | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_lookup_filter_support            | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_lookup_join_filter               | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_lookup_project                   | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_lt                               | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_lte                              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_merge_add_object_id              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_merge_add_object_id              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_merge_documents                  | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_merge_documents_at_path          | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_merge_fail_when_not_matched      | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_merge_filter_support             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_merge_generate_object_id         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_merge_handle_when_matched        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_merge_handle_when_matched        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_merge_join                       | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_not_gt                           | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_not_gte                          | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_not_lt                           | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_not_lte                          | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_project                          | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_project                          | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_project_find                     | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_project_find                     | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_range                            | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_redact                           | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_redact                           | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_replace_root                     | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_replace_root                     | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_selectivity                      | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_dollar_text                             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_exp_moving_avg                          | window | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_expression_get                          | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_expression_get                          | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_expression_map                          | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_expression_partition_by_fields_get      | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_expression_partition_get                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_expression_partition_get                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_expression_partition_get                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_extract_vector                          | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_first_transition                        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_first_transition_on_sorted              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_firstn_transition                       | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_firstn_transition_on_sorted             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_geonear_within_range                    | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_integral_derivative_final               | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_integral_transition                     | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_last_transition                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_last_transition_on_sorted               | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_lastn_transition                        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_lastn_transition_on_sorted              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_linear_fill                             | window | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_locf_fill                               | window | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_maxminn_combine                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_maxminn_final                           | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_maxn_transition                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_merge_objects                           | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_merge_objects_final                     | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_merge_objects_on_sorted                 | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_merge_objects_transition                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_merge_objects_transition_on_sorted      | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_minn_transition                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_orderby                                 | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_orderby_compare                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_orderby_eq                              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_orderby_gt                              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_orderby_lt                              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_orderby_partition                       | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_orderby_partition                       | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_query_match                             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_query_to_tsquery                        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_rank                                    | window | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_rum_composite_ordering                  | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_search_param                            | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_shift                                   | window | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_std_dev_pop_final                       | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_std_dev_pop_samp_combine                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_std_dev_pop_samp_transition             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_std_dev_pop_samp_winfunc_invtransition  | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_std_dev_pop_winfunc_final               | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_std_dev_samp_final                      | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_std_dev_samp_winfunc_final              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_sum_avg_minvtransition                  | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_text_meta_qual                          | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_text_tsquery                            | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_unique_exclusion_index_equal            | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_unique_index_equal                      | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_unique_shard_path_equal                 | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_update_document                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_update_returned_value                   | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_all                        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_bits_all_clear             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_bits_all_set               | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_bits_any_clear             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_bits_any_set               | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_elemmatch                  | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_eq                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_exists                     | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_gt                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_gte                        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_in                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_lt                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_lte                        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_mod                        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_ne                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_nin                        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_regex                      | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_size                       | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bson_value_dollar_type                       | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsoncovariancepop                            | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsoncovariancesamp                           | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonderivative                               | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonfirst                                    | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonfirstn                                   | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonfirstnonsorted                           | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonfirstonsorted                            | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonindexbounds_in                           | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonindexbounds_out                          | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonindexbounds_recv                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonindexbounds_send                         | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonintegral                                 | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonlast                                     | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonlastn                                    | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonlastnonsorted                            | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonlastonsorted                             | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonmaxn                                     | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonmedian                                   | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonminn                                     | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonpercentile                               | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonquery_eq                                 | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonquery_gt                                 | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonquery_gte                                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonquery_lt                                 | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonquery_lte                                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonstddevpop                                | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | bsonstddevsamp                               | agg    | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_composite_path_compare_partial      | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_composite_path_consistent           | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_composite_path_extract_query        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_composite_path_extract_value        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_composite_path_options              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_exclusion_consistent                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_exclusion_extract_query             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_exclusion_extract_value             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_exclusion_options                   | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_hashed_consistent                   | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_hashed_extract_query                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_hashed_extract_value                | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | gin_bson_hashed_options                      | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | validate_dbname                              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | tdigest_add_double                           | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | tdigest_add_double_array                     | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | tdigest_array_percentiles                    | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | tdigest_combine                              | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | tdigest_deserial                             | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | tdigest_percentile                           | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | tdigest_serial                               | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | trigger_validate_dbname                      | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | rum_bson_single_path_extract_tsvector        | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |
| documentdb_api_internal | rum_bson_text_path_options                   | func   | documentdb_api_internal_readonly  | documentdb_admin_role, documentdb_readonly_role |

### Backend Changes

- Create new schemas
  - documentdb_api_v2
  - documentdb_api_internal_readonly
  - documentdb_api_internal_readwrite
  - documentdb_internal_admin
  - documentdb_internal_bgworker.

  ``` psql
  CREATE SCHEMA documentdb_api_v2;

  REVOKE ALL ON SCHEMA documentdb_api_v2 FROM PUBLIC;

  ALTER DEFAULT PRIVILEGES IN SCHEMA documentdb_api_v2
    REVOKE EXECUTE ON FUNCTIONS FROM PUBLIC;
  ```

- Add all documentdb_api methods in the new documentdb_api_v2 schema, repeat the same for documentdb_api_internal_readonly/readwrite/admin/bgworker. These new functions will continue to point to the same C entry points as the old functions. We will do the same for corresponding internal functions and schemas.
  Example below

  ``` sql
  CREATE OR REPLACE FUNCTION documentdb_api_v2.create_user(p_spec __CORE_SCHEMA_V2__.bson)
                                                            RETURNS __CORE_SCHEMA_V2__.bson
  LANGUAGE C
  VOLATILE
  AS 'MODULE_PATHNAME', $function$documentdb_extension_create_user$function$;
  ```

- GRANT EXECUTE on the functions in these schemas to the respective built-in roles.
- Add two GUCs IsRbacCompliantApiSchemaEnabled, IsRbacCompliantInternalSchemaEnabled
- Add new global variables ApiSchemaNameV3, ApiInternalReadOnlySchemaName, ApiInternalReadWriteSchemaName, ApiInternalAdminSchemaName, ApiInternalBgWorkerSchemaName.
- Replace ApiSchemaNameV2 with ApiSchemaNameV3 everywhere in our code base. Similarly replace ApiInternalSchemaNameV2 with ApiInternalReadOnlySchemaName/ApiInternalReadWriteSchemaName/ApiInternalAdminSchemaName/ApiInternalBgWorkerSchemaName.
- Replace the variables with their final names when the GUC is enabled, keep the current names as long as the GUC is disabled

  ``` C
  void
  InitializeDocumentDBApiExtensionCache(void)
  {
    if (CacheValidity == CACHE_VALID)
    {
      return;
    }

    /* Initialize schema names based on feature flags */
    InitializeSchemaNames();
  }
  ```

  ``` C
  /*
  * InitializeSchemaNames sets up the schema name globals based on feature flags
  */
  void
  InitializeSchemaNames(void)
  {
    /* Default name */
      ApiSchemaNameV2 = "documentdb_api";
      ApiInternalReadOnlySchemaName = "documentdb_api_internal";
      ApiInternalReadWriteSchemaName = "documentdb_api_internal";
      ApiInternalAdminSchemaName = "documentdb_api_internal";
      ApiInternalBgWorkerSchemaName = "documentdb_api_internal";
      
      if (IsRbacCompliantSchemaEnabled)
      {
          ApiSchemaNameV2 = "documentdb_api_v2";
      }
      
      if (IsRbacCompliantInternalSchemaEnabled)
      {
          ApiInternalReadOnlySchemaName = "documentdb_api_internal_readonly";
          ApiInternalReadWriteSchemaName = "documentdb_api_internal_readwrite";
          ApiInternalAdminSchemaName = "documentdb_api_internal_admin";
          ApiInternalBgWorkerSchemaName = "documentdb_api_internal_bgworker";
      }
  }
  ```

### Gateway Changes

All calls from the gateway to the backend go through command_funcs.c which in turn calls the methods in oid_cache.c. We will therefore make sure gateway switches over to the new schema by replacing the schema name variables used in oid_cache.c.

### Rollout

We need to rollout this feature over several releases.

Release 1:

- Create the new schemas and add functions to the new schemas
- Introduce new global variables ApiSchemaNameV3, ApiInternalReadOnlySchemaName, ApiInternalReadWriteSchemaName, ApiInternalAdminSchemaName, ApiInternalBgWorkerSchemaName and replace old schema variables with the new variables. The new variables will continue to have the old schema names by default.
- Introduce the new GUCs and code in PGInit to override the schema variable names when the GUCs are set

Release 2:

- Have a full pipeline test run with Flex container generation enabled to validate that nothing is broken with the GUCs turned off
- Have a full pipeline test run with Flex container generation enabled to validate that nothing is broken with the GUCs turned on
- After the release is completed run a release validation run with the GUCs set on a cluster with the new bits, look at telemetry for any new privilege related errors and internal errors.
- Also set the GUCs to true, restart PG on a test cluster and perform a few operations and look at telemetry for any new privilege related errors and internal errors.

Release 3:

- Check in changes to set both GUCs to true
- Monitor telemetry during rollout to look for any new 401, 403, 500 errors
- Verify that everything works as expected on an existing test cluster
- Verify that everything works as expected on a brand new cluster

Release 4:

- If things look good post Release 2, we run the following to cut of all access to the old schemas

``` sql
REVOKE ALL ON SCHEMA documentdb_api FROM PUBLIC;
REVOKE ALL ON SCHEMA documentdb_api_internal FROM PUBLIC;
```

### Sharding

If we were doing this in one release we run the risk of different nodes being at different stages of the upgrade. Since we're doing this over multiple releases we can be assured that all the nodes have the new schema definitions.

## Test Plan

When we set the GUCs to true by default (Release 2) we will also change the test setup scripts to always set the GUCs to true in the test instances.

## Monitoring / Telemetry

We will monitor for any increase in errors corresponding to the error codes 401, 403 and 500.
