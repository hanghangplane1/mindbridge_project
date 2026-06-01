-- MindBridge cloud storage metadata schema.
--
-- File bytes live in FastDFS. Redis owns temporary upload state. MySQL owns
-- durable file metadata and the DistributedStateStore replication tables when
-- MINDBRIDGE_STATE_BACKEND=mysql is enabled.

CREATE TABLE IF NOT EXISTS mb_file_info (
  id BIGINT NOT NULL AUTO_INCREMENT,
  md5 VARCHAR(256) NOT NULL,
  file_id VARCHAR(512) NOT NULL,
  url VARCHAR(1024) NOT NULL,
  size BIGINT NOT NULL DEFAULT 0,
  type VARCHAR(64) NOT NULL DEFAULT '',
  ref_count INT NOT NULL DEFAULT 1,
  status VARCHAR(32) NOT NULL DEFAULT 'active',
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uq_mb_file_md5 (md5(191)),
  KEY idx_mb_file_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS mb_user_file_list (
  id BIGINT NOT NULL AUTO_INCREMENT,
  user_id VARCHAR(128) NOT NULL,
  conversation_id VARCHAR(256) NOT NULL DEFAULT 'default',
  run_id VARCHAR(256) NOT NULL DEFAULT '',
  md5 VARCHAR(256) NOT NULL,
  file_name VARCHAR(512) NOT NULL,
  shared_status INT NOT NULL DEFAULT 0,
  pv INT NOT NULL DEFAULT 0,
  created_at BIGINT NOT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uq_mb_user_file (user_id, conversation_id, md5(191), file_name(191)),
  KEY idx_mb_user_files (user_id, conversation_id, created_at),
  KEY idx_mb_user_run (user_id, run_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS mb_user_file_count (
  user_id VARCHAR(128) NOT NULL,
  file_count INT NOT NULL DEFAULT 0,
  updated_at BIGINT NOT NULL,
  PRIMARY KEY (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS mb_storage_change_tasks (
  task_id BIGINT NOT NULL AUTO_INCREMENT,
  user_id VARCHAR(128) NOT NULL,
  conversation_id VARCHAR(256) NOT NULL DEFAULT 'default',
  run_id VARCHAR(256) NOT NULL DEFAULT '',
  md5 VARCHAR(256) NOT NULL,
  operation VARCHAR(64) NOT NULL DEFAULT 'artifact_upsert',
  payload JSON NOT NULL,
  status VARCHAR(32) NOT NULL DEFAULT 'pending',
  attempt INT NOT NULL DEFAULT 0,
  reason VARCHAR(1024) NOT NULL DEFAULT '',
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL,
  PRIMARY KEY (task_id),
  KEY idx_mb_storage_task_status (status, task_id),
  KEY idx_mb_storage_task_scope (user_id, conversation_id, run_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS mb_storage_node_progress (
  node_id VARCHAR(128) NOT NULL,
  last_applied_task_id BIGINT NOT NULL DEFAULT 0,
  applied_count BIGINT NOT NULL DEFAULT 0,
  skipped_count BIGINT NOT NULL DEFAULT 0,
  updated_at BIGINT NOT NULL,
  PRIMARY KEY (node_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS mb_state_records (
  user_id VARCHAR(128) NOT NULL,
  conversation_id VARCHAR(256) NOT NULL,
  namespace_name VARCHAR(128) NOT NULL,
  state_key VARCHAR(256) NOT NULL,
  version BIGINT NOT NULL,
  risk_level VARCHAR(32) NOT NULL DEFAULT 'low',
  payload JSON NOT NULL,
  updated_at BIGINT NOT NULL,
  PRIMARY KEY (user_id, conversation_id, namespace_name, state_key),
  KEY idx_mb_state_namespace (user_id, conversation_id, namespace_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS mb_state_changes (
  change_id BIGINT NOT NULL AUTO_INCREMENT,
  user_id VARCHAR(128) NOT NULL,
  conversation_id VARCHAR(256) NOT NULL,
  namespace_name VARCHAR(128) NOT NULL,
  state_key VARCHAR(256) NOT NULL,
  operation VARCHAR(32) NOT NULL,
  version BIGINT NOT NULL,
  risk_level VARCHAR(32) NOT NULL DEFAULT 'low',
  payload JSON NOT NULL,
  source_node VARCHAR(128) NOT NULL,
  target_nodes JSON NOT NULL,
  created_at BIGINT NOT NULL,
  PRIMARY KEY (change_id),
  KEY idx_mb_changes_after (change_id),
  KEY idx_mb_changes_scope (user_id, conversation_id, namespace_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS mb_replica_progress (
  node_id VARCHAR(128) NOT NULL,
  last_applied_change_id BIGINT NOT NULL DEFAULT 0,
  applied_count BIGINT NOT NULL DEFAULT 0,
  skipped_count BIGINT NOT NULL DEFAULT 0,
  updated_at BIGINT NOT NULL,
  PRIMARY KEY (node_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
