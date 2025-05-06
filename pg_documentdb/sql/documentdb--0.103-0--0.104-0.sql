#include "udfs/index_mgmt/create_index_background--0.104-0.sql"

-- Schedule the index build task
DO LANGUAGE plpgsql $cmd$
BEGIN
    PERFORM __API_SCHEMA_INTERNAL__.schedule_background_index_build_jobs();
END;
$cmd$;