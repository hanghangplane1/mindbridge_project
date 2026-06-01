#!/usr/bin/env bash
set -euo pipefail

HOST_IP="$(hostname -i | awk '{print $1}')"
sed -i "s/^tracker_server=.*/tracker_server=${HOST_IP}:22122/" /etc/fdfs/storage.conf /etc/fdfs/client.conf /etc/fdfs/mod_fastdfs.conf

mkdir -p /etc/nginx/ssl
if [[ ! -f /etc/nginx/ssl/server.crt || ! -f /etc/nginx/ssl/server.key ]]; then
  openssl req -x509 -nodes -days 3650 \
    -newkey rsa:2048 \
    -keyout /etc/nginx/ssl/server.key \
    -out /etc/nginx/ssl/server.crt \
    -subj "/C=CN/ST=Beijing/L=Beijing/O=MindBridge/CN=mindbridge_fastdfs" \
    -addext "subjectAltName=DNS:mindbridge_fastdfs,IP:127.0.0.1,IP:${HOST_IP}" >/dev/null 2>&1
fi

/usr/bin/fdfs_trackerd /etc/fdfs/tracker.conf start
sleep 2
/usr/bin/fdfs_storaged /etc/fdfs/storage.conf start
sleep 8

exec /usr/local/nginx/sbin/nginx -g "daemon off;"
