#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="${MINDBRIDGE_K8S_IMAGE:-mindbridge:platform-dev}"
NAMESPACE="${MINDBRIDGE_K8S_NAMESPACE:-mindbridge}"

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: missing required command: $1" >&2
    exit 1
  }
}

need kubectl
need docker

cd "$ROOT_DIR"

docker build -t "$IMAGE_NAME" .

kubectl apply -f k8s/base/namespace.yaml

if [[ -n "${DASHSCOPE_API_KEY:-}" ]]; then
  kubectl create secret generic mindbridge-secrets \
    --namespace "$NAMESPACE" \
    --from-literal=DASHSCOPE_API_KEY="$DASHSCOPE_API_KEY" \
    --dry-run=client -o yaml | kubectl apply -f -
else
  echo "WARN: DASHSCOPE_API_KEY is not set; remote model startup may fail." >&2
fi

kubectl apply -f k8s/base/configmap.yaml
kubectl apply -f k8s/base/storage.yaml
kubectl apply -f k8s/base/platform.yaml
kubectl apply -f k8s/base/workloads.yaml
kubectl apply -f k8s/base/services.yaml

if [[ "${MINDBRIDGE_K8S_DEV_NODEPORT:-1}" == "1" ]]; then
  kubectl apply -f k8s/profiles/dev-k3s/nodeport.yaml
fi

echo "MindBridge platform manifests applied."
echo "Check: kubectl get pods -n $NAMESPACE"
echo "Dev platform URL: http://127.0.0.1:30077/api/platform/health"
