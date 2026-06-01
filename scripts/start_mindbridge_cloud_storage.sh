#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

COMPOSE_FILE="infra/cloud_storage/docker-compose.yaml"
ENV_DIR=".mindbridge/cloud_storage"
CLIENT_DIR="$ENV_DIR/fdfs_client"
ENV_FILE="$ENV_DIR/live.env"

mkdir -p "$CLIENT_DIR"

docker compose -f "$COMPOSE_FILE" up -d --build mindbridge_mysql mindbridge_redis mindbridge_fastdfs

for container in mindbridge_mysql mindbridge_redis mindbridge_fastdfs; do
  for _ in $(seq 1 90); do
    health="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}running{{end}}' "$container")"
    if [[ "$health" == "healthy" || "$health" == "running" ]]; then
      break
    fi
    sleep 1
  done
  health="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}running{{end}}' "$container")"
  if [[ "$health" != "healthy" && "$health" != "running" ]]; then
    echo "FAIL: $container is $health"
    docker logs --tail 120 "$container" || true
    exit 1
  fi
done

FASTDFS_IP="$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' mindbridge_fastdfs)"
cat >"$CLIENT_DIR/client.conf" <<EOF
connect_timeout=30
network_timeout=60
base_path=$ROOT_DIR/$CLIENT_DIR
tracker_server=$FASTDFS_IP:22122
log_level=error
use_connection_pool=false
load_fdfs_parameters_from_tracker=false
use_storage_id=false
storage_ids_filename=storage_ids.conf
http.tracker_server_port=80
EOF

cat >"$ENV_FILE" <<EOF
export MINDBRIDGE_MYSQL_HOST=127.0.0.1
export MINDBRIDGE_MYSQL_PORT=${MINDBRIDGE_MYSQL_PORT:-3307}
export MINDBRIDGE_MYSQL_USER=${MINDBRIDGE_MYSQL_USER:-mindbridge}
export MINDBRIDGE_MYSQL_PASSWORD=${MINDBRIDGE_MYSQL_PASSWORD:-mindbridge}
export MINDBRIDGE_MYSQL_DATABASE=${MINDBRIDGE_MYSQL_DATABASE:-mindbridge}
export MINDBRIDGE_MYSQL_CONTAINER=mindbridge_mysql
export MINDBRIDGE_REDIS_HOST=127.0.0.1
export MINDBRIDGE_REDIS_PORT=${MINDBRIDGE_REDIS_PORT:-6379}
export MINDBRIDGE_FASTDFS_CLIENT_CONF=$ROOT_DIR/$CLIENT_DIR/client.conf
export MINDBRIDGE_FASTDFS_STORAGE_BASE_URL=http://127.0.0.1:${MINDBRIDGE_FASTDFS_HTTP_PORT:-80}
export MINDBRIDGE_FASTDFS_STORAGE_HTTPS_BASE_URL=https://127.0.0.1:${MINDBRIDGE_FASTDFS_HTTPS_PORT:-443}
export MINDBRIDGE_STORAGE_BACKEND=cloud
export MINDBRIDGE_STATE_BACKEND=mysql
EOF

echo "PASS: MindBridge cloud storage stack is running"
echo "source $ENV_FILE"
