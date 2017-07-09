/* citus--7.0-2--7.0-3.sql */

SET search_path = 'pg_catalog';

CREATE FUNCTION assign_distributed_transaction_id(initiatorNodeIdentifier int8, transactionId int8, transactionStamp timestamptz)
     RETURNS void
     LANGUAGE C STRICT
     AS 'citus',$$assign_distributed_transaction_id$$;
 COMMENT ON FUNCTION assign_distributed_transaction_id(originNodeId int8, transactionId int8, transactionStamp timestamptz)
     IS 'sets the distributed transaction id';

CREATE OR REPLACE FUNCTION get_distributed_transaction_id()
     RETURNS record
     LANGUAGE C STRICT
     AS 'citus',$$get_distributed_transaction_id$$;
 COMMENT ON FUNCTION get_distributed_transaction_id()
     IS 'gets the current distributed transaction id';

CREATE OR REPLACE FUNCTION get_next_distributed_transaction_id()
     RETURNS int8
     LANGUAGE C STRICT
     AS 'citus',$$get_next_distributed_transaction_id$$;
 COMMENT ON FUNCTION get_next_distributed_transaction_id()
     IS 'gets the next distributed transaction id';

RESET search_path;