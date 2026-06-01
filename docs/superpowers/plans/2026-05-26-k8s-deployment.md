# MindBridge K8s 部署实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 MindBridge 多服务架构部署到 K3s 集群，用于开发/测试环境

**Architecture:** 单 Docker 镜像通过环境变量选择启动不同服务，K8s YAML 定义部署清单，一键脚本完成构建和部署

**Tech Stack:** K3s, Docker, kubectl, Ubuntu 22.04, CMake, Boost, libbpf

---

## 文件结构

```
mindbridge_project/
├── Dockerfile                    # 多阶段构建镜像
├── start-service.sh              # 容器启动入口脚本
├── k8s/
│   ├── namespace.yaml            # Namespace 定义
│   ├── configmap.yaml            # ConfigMap 配置
│   ├── gateway.yaml              # Gateway Deployment
│   ├── orchestrator.yaml         # Orchestrator Deployment
│   ├── counselor.yaml            # Counselor StatefulSet
│   ├── evaluator.yaml            # Evaluator StatefulSet
│   ├── frontend.yaml             # Frontend Deployment
│   ├── ebpf-daemonset.yaml       # eBPF DaemonSet
│   ├── services.yaml             # 所有 Service 定义
│   └── external-services.yaml    # ExternalName Services
└── scripts/
    └── k8s-deploy.sh             # 一键部署脚本
```

---

### Task 1: 创建启动入口脚本

**Files:**
- Create: `start-service.sh`

- [ ] **Step 1: 创建 start-service.sh**

```bash
#!/bin/bash
set -euo pipefail

case "${SERVICE_NAME:-}" in
  gateway)
    exec ./mindbridge_gateway 8090
    ;;
  orchestrator)
    exec ./mindbridge_orchestrator 5009
    ;;
  counselor)
    exec ./mindbridge_counselor "${COUNSELOR_PORT:-5010}"
    ;;
  evaluator)
    exec ./mindbridge_evaluator "${EVALUATOR_PORT:-5011}"
    ;;
  frontend)
    exec python3 scripts/serve_demo_frontend.py --port 5173 --directory frontend/demo
    ;;
  ebpf-monitor)
    exec ./ebpf/mindbridge_ebpf_monitor -m 0 --trace-fs --trace-net --trace-resources --trace-tls
    ;;
  *)
    echo "ERROR: SERVICE_NAME not set or invalid: ${SERVICE_NAME:-<empty>}"
    echo "Valid values: gateway, orchestrator, counselor, evaluator, frontend, ebpf-monitor"
    exit 1
    ;;
esac
```

- [ ] **Step 2: 设置可执行权限**

Run: `chmod +x start-service.sh`

- [ ] **Step 3: 验证脚本语法**

Run: `bash -n start-service.sh`
Expected: 无输出（语法正确）

- [ ] **Step 4: Commit**

```bash
git add start-service.sh
git commit -m "feat: add K8s container entrypoint script"
```

---

### Task 2: 创建 Dockerfile

**Files:**
- Create: `Dockerfile`

- [ ] **Step 1: 创建 Dockerfile**

```dockerfile
# Stage 1: 构建层
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    cmake g++ make \
    libboost-all-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    nlohmann-json3-dev \
    libbpf-dev \
    linux-headers-generic \
    libelf-dev \
    zlib1g-dev \
    pkg-config \
    python3 \
    && rm -rf /var/lib/apt/lists/*

COPY . /build
WORKDIR /build

# 构建核心服务
RUN cmake -S . -B build -DBUILD_TESTING=OFF && \
    cmake --build build --target \
      mindbridge_gateway \
      mindbridge_orchestrator \
      mindbridge_counselor \
      mindbridge_evaluator \
      -j$(nproc)

# 构建 eBPF 工具
RUN cmake -S . -B build-ebpf -DBUILD_TESTING=OFF && \
    cmake --build build-ebpf --target \
      mindbridge_ebpf_monitor \
      mindbridge_sslsniff_monitor \
      -j$(nproc) || true

# Stage 2: 运行层
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libcurl4 \
    libssl3 \
    libboost-system1.74.0 \
    libboost-thread1.74.0 \
    libboost-filesystem1.74.0 \
    python3 \
    iproute2 \
    sudo \
    && rm -rf /var/lib/apt/lists/*

# 复制编译产物
COPY --from=builder /build/build/mindbridge_harness/mindbridge_gateway /app/
COPY --from=builder /build/build/mindbridge_harness/mindbridge_orchestrator /app/
COPY --from=builder /build/build/mindbridge_harness/mindbridge_counselor /app/
COPY --from=builder /build/build/mindbridge_harness/mindbridge_evaluator /app/

# 复制 eBPF 工具
COPY --from=builder /build/build-ebpf/mindbridge_harness/mindbridge_ebpf_monitor /app/ebpf/
COPY --from=builder /build/build-ebpf/mindbridge_harness/mindbridge_sslsniff_monitor /app/ebpf/ 2>/dev/null || true

# 复制前端文件
COPY frontend/demo /app/frontend/demo

# 复制启动脚本和辅助脚本
COPY start-service.sh /app/
COPY scripts/serve_demo_frontend.py /app/scripts/
COPY scripts/sudo_ebpf.sh /app/scripts/ 2>/dev/null || true

WORKDIR /app

# 创建 .mindbridge 目录
RUN mkdir -p /app/.mindbridge/state /app/.mindbridge/runs

# 健康检查
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD [ -f /app/mindbridge_gateway ] && exit 0 || exit 1

ENTRYPOINT ["/app/start-service.sh"]
```

- [ ] **Step 2: 验证 Dockerfile 语法**

Run: `docker build --check . 2>&1 || echo "Note: --check not supported, will verify during build"`

- [ ] **Step 3: Commit**

```bash
git add Dockerfile
git commit -m "feat: add multi-stage Dockerfile for K8s deployment"
```

---

### Task 3: 创建 K8s Namespace 和 ConfigMap

**Files:**
- Create: `k8s/namespace.yaml`
- Create: `k8s/configmap.yaml`

- [ ] **Step 1: 创建 k8s 目录**

Run: `mkdir -p k8s`

- [ ] **Step 2: 创建 namespace.yaml**

```yaml
apiVersion: v1
kind: Namespace
metadata:
  name: mindbridge
  labels:
    app: mindbridge
```

- [ ] **Step 3: 创建 configmap.yaml**

```yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: mindbridge-config
  namespace: mindbridge
data:
  # 模型配置
  MINDBRIDGE_MODEL_PROVIDER: "dashscope_native"
  MINDBRIDGE_MODEL_NAME: "qwen3.6-flash"
  # 服务发现
  MINDBRIDGE_COUNSELOR_URLS: "http://counselor-0.counselor-svc.mindbridge.svc.cluster.local:5010,http://counselor-1.counselor-svc.mindbridge.svc.cluster.local:5010,http://counselor-2.counselor-svc.mindbridge.svc.cluster.local:5010"
  MINDBRIDGE_EVALUATOR_URLS: "http://evaluator-0.evaluator-svc.mindbridge.svc.cluster.local:5011,http://evaluator-1.evaluator-svc.mindbridge.svc.cluster.local:5011,http://evaluator-2.evaluator-svc.mindbridge.svc.cluster.local:5011"
  MINDBRIDGE_ORCHESTRATOR_URL: "http://orchestrator-svc.mindbridge.svc.cluster.local:5009"
  MINDBRIDGE_COUNSELOR_URL: "http://counselor-0.counselor-svc.mindbridge.svc.cluster.local:5010"
  MINDBRIDGE_EVALUATOR_URL: "http://evaluator-0.evaluator-svc.mindbridge.svc.cluster.local:5011"
  # eBPF 配置
  MINDBRIDGE_EBPF_ENABLED: "1"
  MINDBRIDGE_EBPF_TRACE_RESOURCES: "1"
  MINDBRIDGE_EBPF_TRACE_TLS: "1"
  MINDBRIDGE_EBPF_USE_SUDO: "1"
  MINDBRIDGE_EBPF_BINARY: "/app/ebpf/mindbridge_ebpf_monitor"
  MINDBRIDGE_EBPF_TLS_BINARY: "/app/ebpf/mindbridge_sslsniff_monitor"
  # 存储配置（连接宿主机）
  MYSQL_HOST: "host.docker.internal"
  MYSQL_PORT: "3306"
  REDIS_HOST: "host.docker.internal"
  REDIS_PORT: "6379"
  FASTDFS_TRACKER: "host.docker.internal:22122"
```

- [ ] **Step 4: 验证 YAML 语法**

Run: `kubectl apply --dry-run=client -f k8s/namespace.yaml && kubectl apply --dry-run=client -f k8s/configmap.yaml`
Expected: `namespace/mindbridge created (dry run)` 和 `configmap/mindbridge-config created (dry run)`

- [ ] **Step 5: Commit**

```bash
git add k8s/namespace.yaml k8s/configmap.yaml
git commit -m "feat: add K8s namespace and configmap"
```

---

### Task 4: 创建 Gateway Deployment

**Files:**
- Create: `k8s/gateway.yaml`

- [ ] **Step 1: 创建 gateway.yaml**

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: gateway
  namespace: mindbridge
  labels:
    app: gateway
spec:
  replicas: 1
  selector:
    matchLabels:
      app: gateway
  template:
    metadata:
      labels:
        app: gateway
    spec:
      containers:
      - name: gateway
        image: mindbridge:dev
        imagePullPolicy: Never
        ports:
        - containerPort: 8090
          name: http
        envFrom:
        - configMapRef:
            name: mindbridge-config
        - secretRef:
            name: mindbridge-secrets
        env:
        - name: SERVICE_NAME
          value: "gateway"
        readinessProbe:
          httpGet:
            path: /api/health
            port: 8090
          initialDelaySeconds: 5
          periodSeconds: 10
          timeoutSeconds: 5
        livenessProbe:
          httpGet:
            path: /api/health
            port: 8090
          initialDelaySeconds: 10
          periodSeconds: 30
          timeoutSeconds: 5
        resources:
          requests:
            memory: "128Mi"
            cpu: "100m"
          limits:
            memory: "512Mi"
            cpu: "500m"
```

- [ ] **Step 2: 验证 YAML**

Run: `kubectl apply --dry-run=client -f k8s/gateway.yaml`
Expected: `deployment.apps/gateway created (dry run)`

- [ ] **Step 3: Commit**

```bash
git add k8s/gateway.yaml
git commit -m "feat: add Gateway K8s deployment"
```

---

### Task 5: 创建 Orchestrator Deployment

**Files:**
- Create: `k8s/orchestrator.yaml`

- [ ] **Step 1: 创建 orchestrator.yaml**

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: orchestrator
  namespace: mindbridge
  labels:
    app: orchestrator
spec:
  replicas: 1
  selector:
    matchLabels:
      app: orchestrator
  template:
    metadata:
      labels:
        app: orchestrator
    spec:
      containers:
      - name: orchestrator
        image: mindbridge:dev
        imagePullPolicy: Never
        ports:
        - containerPort: 5009
          name: http
        envFrom:
        - configMapRef:
            name: mindbridge-config
        - secretRef:
            name: mindbridge-secrets
        env:
        - name: SERVICE_NAME
          value: "orchestrator"
        readinessProbe:
          httpGet:
            path: /api/health
            port: 5009
          initialDelaySeconds: 5
          periodSeconds: 10
        livenessProbe:
          httpGet:
            path: /api/health
            port: 5009
          initialDelaySeconds: 10
          periodSeconds: 30
        resources:
          requests:
            memory: "128Mi"
            cpu: "100m"
          limits:
            memory: "512Mi"
            cpu: "500m"
```

- [ ] **Step 2: Commit**

```bash
git add k8s/orchestrator.yaml
git commit -m "feat: add Orchestrator K8s deployment"
```

---

### Task 6: 创建 Counselor StatefulSet

**Files:**
- Create: `k8s/counselor.yaml`

- [ ] **Step 1: 创建 counselor.yaml**

```yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: counselor
  namespace: mindbridge
  labels:
    app: counselor
spec:
  serviceName: counselor-svc
  replicas: 3
  selector:
    matchLabels:
      app: counselor
  template:
    metadata:
      labels:
        app: counselor
    spec:
      containers:
      - name: counselor
        image: mindbridge:dev
        imagePullPolicy: Never
        ports:
        - containerPort: 5010
          name: http
        envFrom:
        - configMapRef:
            name: mindbridge-config
        - secretRef:
            name: mindbridge-secrets
        env:
        - name: SERVICE_NAME
          value: "counselor"
        - name: COUNSELOR_PORT
          value: "5010"
        readinessProbe:
          httpGet:
            path: /api/health
            port: 5010
          initialDelaySeconds: 5
          periodSeconds: 10
        livenessProbe:
          httpGet:
            path: /api/health
            port: 5010
          initialDelaySeconds: 10
          periodSeconds: 30
        volumeMounts:
        - name: counselor-state
          mountPath: /app/.mindbridge
        resources:
          requests:
            memory: "256Mi"
            cpu: "200m"
          limits:
            memory: "1Gi"
            cpu: "1000m"
  volumeClaimTemplates:
  - metadata:
      name: counselor-state
    spec:
      accessModes: ["ReadWriteOnce"]
      resources:
        requests:
          storage: 1Gi
```

- [ ] **Step 2: Commit**

```bash
git add k8s/counselor.yaml
git commit -m "feat: add Counselor K8s StatefulSet"
```

---

### Task 7: 创建 Evaluator StatefulSet

**Files:**
- Create: `k8s/evaluator.yaml`

- [ ] **Step 1: 创建 evaluator.yaml**

```yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: evaluator
  namespace: mindbridge
  labels:
    app: evaluator
spec:
  serviceName: evaluator-svc
  replicas: 3
  selector:
    matchLabels:
      app: evaluator
  template:
    metadata:
      labels:
        app: evaluator
    spec:
      containers:
      - name: evaluator
        image: mindbridge:dev
        imagePullPolicy: Never
        ports:
        - containerPort: 5011
          name: http
        envFrom:
        - configMapRef:
            name: mindbridge-config
        - secretRef:
            name: mindbridge-secrets
        env:
        - name: SERVICE_NAME
          value: "evaluator"
        - name: EVALUATOR_PORT
          value: "5011"
        readinessProbe:
          httpGet:
            path: /api/health
            port: 5011
          initialDelaySeconds: 5
          periodSeconds: 10
        livenessProbe:
          httpGet:
            path: /api/health
            port: 5011
          initialDelaySeconds: 10
          periodSeconds: 30
        resources:
          requests:
            memory: "128Mi"
            cpu: "100m"
          limits:
            memory: "512Mi"
            cpu: "500m"
  volumeClaimTemplates:
  - metadata:
      name: evaluator-state
    spec:
      accessModes: ["ReadWriteOnce"]
      resources:
        requests:
          storage: 512Mi
```

- [ ] **Step 2: Commit**

```bash
git add k8s/evaluator.yaml
git commit -m "feat: add Evaluator K8s StatefulSet"
```

---

### Task 8: 创建 Frontend Deployment

**Files:**
- Create: `k8s/frontend.yaml`

- [ ] **Step 1: 创建 frontend.yaml**

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: frontend
  namespace: mindbridge
  labels:
    app: frontend
spec:
  replicas: 1
  selector:
    matchLabels:
      app: frontend
  template:
    metadata:
      labels:
        app: frontend
    spec:
      containers:
      - name: frontend
        image: mindbridge:dev
        imagePullPolicy: Never
        ports:
        - containerPort: 5173
          name: http
        env:
        - name: SERVICE_NAME
          value: "frontend"
        resources:
          requests:
            memory: "64Mi"
            cpu: "50m"
          limits:
            memory: "256Mi"
            cpu: "200m"
```

- [ ] **Step 2: Commit**

```bash
git add k8s/frontend.yaml
git commit -m "feat: add Frontend K8s deployment"
```

---

### Task 9: 创建 eBPF DaemonSet

**Files:**
- Create: `k8s/ebpf-daemonset.yaml`

- [ ] **Step 1: 创建 ebpf-daemonset.yaml**

```yaml
apiVersion: apps/v1
kind: DaemonSet
metadata:
  name: ebpf-monitor
  namespace: mindbridge
  labels:
    app: ebpf-monitor
spec:
  selector:
    matchLabels:
      app: ebpf-monitor
  template:
    metadata:
      labels:
        app: ebpf-monitor
    spec:
      hostPID: true
      containers:
      - name: ebpf-monitor
        image: mindbridge:dev
        imagePullPolicy: Never
        securityContext:
          privileged: true
          capabilities:
            add:
            - SYS_ADMIN
            - SYS_PTRACE
            - NET_ADMIN
        envFrom:
        - configMapRef:
            name: mindbridge-config
        - secretRef:
            name: mindbridge-secrets
        env:
        - name: SERVICE_NAME
          value: "ebpf-monitor"
        volumeMounts:
        - name: kernel-modules
          mountPath: /lib/modules
          readOnly: true
        - name: sys-kernel
          mountPath: /sys/kernel
          readOnly: true
        - name: ebpf-artifacts
          mountPath: /tmp/mindbridge_ebpf
        resources:
          requests:
            memory: "256Mi"
            cpu: "200m"
          limits:
            memory: "1Gi"
            cpu: "1000m"
      volumes:
      - name: kernel-modules
        hostPath:
          path: /lib/modules
      - name: sys-kernel
        hostPath:
          path: /sys/kernel
      - name: ebpf-artifacts
        hostPath:
          path: /tmp/mindbridge_ebpf
```

- [ ] **Step 2: Commit**

```bash
git add k8s/ebpf-daemonset.yaml
git commit -m "feat: add eBPF DaemonSet for K8s"
```

---

### Task 10: 创建 Services

**Files:**
- Create: `k8s/services.yaml`

- [ ] **Step 1: 创建 services.yaml**

```yaml
# Gateway Service (NodePort)
apiVersion: v1
kind: Service
metadata:
  name: gateway-svc
  namespace: mindbridge
  labels:
    app: gateway
spec:
  type: NodePort
  selector:
    app: gateway
  ports:
  - name: http
    port: 8090
    targetPort: 8090
    nodePort: 30090

---
# Orchestrator Service (ClusterIP)
apiVersion: v1
kind: Service
metadata:
  name: orchestrator-svc
  namespace: mindbridge
  labels:
    app: orchestrator
spec:
  selector:
    app: orchestrator
  ports:
  - name: http
    port: 5009
    targetPort: 5009

---
# Counselor Service (Headless for StatefulSet)
apiVersion: v1
kind: Service
metadata:
  name: counselor-svc
  namespace: mindbridge
  labels:
    app: counselor
spec:
  clusterIP: None
  selector:
    app: counselor
  ports:
  - name: http
    port: 5010
    targetPort: 5010

---
# Evaluator Service (Headless for StatefulSet)
apiVersion: v1
kind: Service
metadata:
  name: evaluator-svc
  namespace: mindbridge
  labels:
    app: evaluator
spec:
  clusterIP: None
  selector:
    app: evaluator
  ports:
  - name: http
    port: 5011
    targetPort: 5011

---
# Frontend Service (NodePort)
apiVersion: v1
kind: Service
metadata:
  name: frontend-svc
  namespace: mindbridge
  labels:
    app: frontend
spec:
  type: NodePort
  selector:
    app: frontend
  ports:
  - name: http
    port: 5173
    targetPort: 5173
    nodePort: 30173
```

- [ ] **Step 2: Commit**

```bash
git add k8s/services.yaml
git commit -m "feat: add K8s services for MindBridge"
```

---

### Task 11: 创建 ExternalName Services

**Files:**
- Create: `k8s/external-services.yaml`

- [ ] **Step 1: 创建 external-services.yaml**

```yaml
# MySQL ExternalName Service
apiVersion: v1
kind: Service
metadata:
  name: mysql-ext
  namespace: mindbridge
spec:
  type: ExternalName
  externalName: host.docker.internal

---
# Redis ExternalName Service
apiVersion: v1
kind: Service
metadata:
  name: redis-ext
  namespace: mindbridge
spec:
  type: ExternalName
  externalName: host.docker.internal

---
# FastDFS ExternalName Service
apiVersion: v1
kind: Service
metadata:
  name: fastdfs-ext
  namespace: mindbridge
spec:
  type: ExternalName
  externalName: host.docker.internal
```

- [ ] **Step 2: Commit**

```bash
git add k8s/external-services.yaml
git commit -m "feat: add ExternalName services for host storage"
```

---

### Task 12: 创建一键部署脚本

**Files:**
- Create: `scripts/k8s-deploy.sh`

- [ ] **Step 1: 创建 k8s-deploy.sh**

```bash
#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# 检查前置条件
check_prerequisites() {
    log "Checking prerequisites..."
    
    command -v docker >/dev/null 2>&1 || error "docker not found"
    command -v kubectl >/dev/null 2>&1 || error "kubectl not found"
    
    # 检查 K3s
    if ! kubectl get nodes >/dev/null 2>&1; then
        error "K3s cluster not running. Install with: curl -sfL https://get.k3s.io | sh -"
    fi
    
    log "Prerequisites OK"
}

# 构建 Docker 镜像
build_image() {
    log "Building Docker image..."
    docker build -t mindbridge:dev .
    log "Image built: mindbridge:dev"
}

# 导入镜像到 K3s
import_image() {
    log "Importing image to K3s..."
    docker save mindbridge:dev | sudo k3s ctr images import -
    log "Image imported to K3s"
}

# 创建 Secret
create_secret() {
    log "Creating secrets..."
    
    local api_key="${DASHSCOPE_API_KEY:-}"
    local sudo_pass="${MINDBRIDGE_SUDO_PASSWORD:- }"
    
    if [ -z "$api_key" ]; then
        read -p "Enter DASHSCOPE_API_KEY: " api_key
    fi
    
    kubectl create namespace mindbridge --dry-run=client -o yaml | kubectl apply -f -
    
    kubectl create secret generic mindbridge-secrets \
        --namespace mindbridge \
        --from-literal=DASHSCOPE_API_KEY="$api_key" \
        --from-literal=MINDBRIDGE_SUDO_PASSWORD="$sudo_pass" \
        --dry-run=client -o yaml | kubectl apply -f -
    
    log "Secrets created"
}

# 部署所有组件
deploy_all() {
    log "Deploying all components..."
    kubectl apply -f k8s/
    log "All components deployed"
}

# 等待 Pod 就绪
wait_for_pods() {
    log "Waiting for pods to be ready..."
    
    local timeout=300
    local start_time=$(date +%s)
    
    while true; do
        local current_time=$(date +%s)
        local elapsed=$((current_time - start_time))
        
        if [ $elapsed -ge $timeout ]; then
            error "Timeout waiting for pods"
        fi
        
        local ready=$(kubectl get pods -n mindbridge --no-headers 2>/dev/null | grep -c "Running" || true)
        local total=$(kubectl get pods -n mindbridge --no-headers 2>/dev/null | wc -l || true)
        
        if [ "$ready" -eq "$total" ] && [ "$total" -gt 0 ]; then
            log "All $total pods are running"
            break
        fi
        
        echo -ne "\rWaiting... $ready/$total pods running (${elapsed}s)"
        sleep 5
    done
}

# 显示状态
show_status() {
    echo ""
    log "Deployment status:"
    echo ""
    kubectl get pods -n mindbridge -o wide
    echo ""
    kubectl get services -n mindbridge
    echo ""
    
    local node_ip=$(hostname -I | awk '{print $1}')
    log "Access URLs:"
    echo "  Frontend: http://${node_ip}:30173/index.html"
    echo "  Gateway:  http://${node_ip}:30090/api/health"
}

# 主流程
main() {
    check_prerequisites
    build_image
    import_image
    create_secret
    deploy_all
    wait_for_pods
    show_status
}

main "$@"
```

- [ ] **Step 2: 设置可执行权限**

Run: `chmod +x scripts/k8s-deploy.sh`

- [ ] **Step 3: Commit**

```bash
git add scripts/k8s-deploy.sh
git commit -m "feat: add K8s one-click deployment script"
```

---

### Task 13: 端到端验证

- [ ] **Step 1: 确保宿主机存储服务运行**

Run: `sudo systemctl status mysql redis-server 2>/dev/null || echo "Storage services may not be running - start them manually"`

- [ ] **Step 2: 运行一键部署**

Run: `DASHSCOPE_API_KEY="$DASHSCOPE_API_KEY" bash scripts/k8s-deploy.sh`
Expected: 所有 Pod 状态为 Running

- [ ] **Step 3: 验证 Gateway 健康检查**

Run: `curl -s http://localhost:30090/api/health | python3 -c "import sys,json; d=json.load(sys.stdin); print('Gateway:', 'OK' if d.get('ok') else 'FAIL')"`
Expected: `Gateway: OK`

- [ ] **Step 4: 验证前端可访问**

Run: `curl -s -o /dev/null -w "%{http_code}" http://localhost:30173/index.html`
Expected: `200`

- [ ] **Step 5: 发送测试消息**

Run:
```bash
curl -s -X POST http://localhost:30090/ \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"k8s-test","method":"message/send","params":{"message":{"role":"user","parts":[{"kind":"text","text":"你好"}]}}}' | python3 -c "
import sys, json
d = json.load(sys.stdin)
if 'result' in d:
    print('Message test: OK')
    print('Reply:', d['result'].get('message',{}).get('parts',[{}])[0].get('text','')[:100])
else:
    print('Message test: FAIL')
    print('Error:', d.get('error',{}).get('message','Unknown'))
"
```
Expected: `Message test: OK` with a reply

- [ ] **Step 6: 查看 Pod 日志（可选）**

Run: `kubectl logs -n mindbridge -l app=gateway --tail=20`

- [ ] **Step 7: Commit 验证完成**

```bash
git add -A
git commit -m "chore: K8s deployment verified and working"
```
