# MindBridge K8s 部署设计

## 概述

将 MindBridge 多服务架构部署到 K3s 集群，用于开发/测试环境。核心服务在 K8s 中运行，存储组件（MySQL/Redis/FastDFS）保留在宿主机。

## 目标

- 在 K3s 集群中部署 MindBridge 全部核心服务
- 保留完整项目功能（包括云存储、eBPF 监控）
- 代码零修改，仅添加 K8s 部署配置
- 提供一键部署脚本

## 架构

```
K3s Cluster (mindbridge namespace)
├── Deployments
│   ├── gateway          (1 replica, port 8090)
│   ├── orchestrator     (1 replica, port 5009)
│   ├── counselor        (3 replicas, ports 5010/5012/5013)
│   ├── evaluator        (3 replicas, ports 5011/5014/5015)
│   └── frontend         (1 replica, port 5173)
├── DaemonSet
│   └── ebpf-monitor     (privileged, hostPID)
├── Services (ClusterIP)
│   ├── gateway-svc      → 8090
│   ├── orchestrator-svc → 5009
│   ├── counselor-svc    → 5010,5012,5013
│   ├── evaluator-svc    → 5011,5014,5015
│   └── frontend-svc     → 5173
├── ExternalName Services (连接宿主机存储)
│   ├── mysql-ext        → host.docker.internal:3306
│   ├── redis-ext        → host.docker.internal:6379
│   └── fastdfs-ext      → host.docker.internal:22122
└── ConfigMap + Secret
    ├── mindbridge-config (环境变量)
    └── mindbridge-secrets (API Key 等)
```

## Docker 镜像

### 多阶段构建

```dockerfile
# Stage 1: 构建层
FROM ubuntu:22.04 AS builder
RUN apt-get update && apt-get install -y \
    cmake g++ libboost-all-dev libcurl4-openssl-dev \
    libssl-dev nlohmann-json3-dev libbpf-dev linux-headers-generic
COPY . /build
WORKDIR /build
RUN cmake -S . -B build -DBUILD_TESTING=OFF && \
    cmake --build build -j$(nproc)

# Stage 2: 运行层
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y \
    libcurl4 libssl3 libboost-system1.74.0 python3 iproute2
COPY --from=builder /build/build/mindbridge_harness/ /app/
COPY --from=builder /build/build-ebpf/mindbridge_harness/ /app/ebpf/
COPY frontend/demo /app/frontend/demo
WORKDIR /app
ENTRYPOINT ["/app/start-service.sh"]
```

### 镜像标签

- `mindbridge:dev` - 开发环境镜像
- 单镜像通过环境变量选择启动哪个服务

## 配置管理

### ConfigMap

```yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: mindbridge-config
  namespace: mindbridge
data:
  MINDBRIDGE_MODEL_PROVIDER: "dashscope_native"
  MINDBRIDGE_MODEL_NAME: "qwen3.6-flash"
  MINDBRIDGE_COUNSELOR_URLS: "http://counselor-svc:5010,http://counselor-svc:5012,http://counselor-svc:5013"
  MINDBRIDGE_EVALUATOR_URLS: "http://evaluator-svc:5011,http://evaluator-svc:5014,http://evaluator-svc:5015"
  MINDBRIDGE_ORCHESTRATOR_URL: "http://orchestrator-svc:5009"
  MINDBRIDGE_EBPF_ENABLED: "1"
  MINDBRIDGE_EBPF_TRACE_RESOURCES: "1"
  MINDBRIDGE_EBPF_TRACE_TLS: "1"
  MINDBRIDGE_EBPF_USE_SUDO: "1"
  MINDBRIDGE_EBPF_BINARY: "/app/ebpf/mindbridge_ebpf_monitor"
  MINDBRIDGE_EBPF_TLS_BINARY: "/app/ebpf/mindbridge_sslsniff_monitor"
  MYSQL_HOST: "host.docker.internal"
  MYSQL_PORT: "3306"
  REDIS_HOST: "host.docker.internal"
  REDIS_PORT: "6379"
  FASTDFS_TRACKER: "host.docker.internal:22122"
```

### Secret

```yaml
apiVersion: v1
kind: Secret
metadata:
  name: mindbridge-secrets
  namespace: mindbridge
type: Opaque
data:
  DASHSCOPE_API_KEY: <base64>
  MINDBRIDGE_SUDO_PASSWORD: <base64>
  MYSQL_PASSWORD: <base64>
```

## 部署清单

### 1. Namespace

```yaml
apiVersion: v1
kind: Namespace
metadata:
  name: mindbridge
```

### 2. Gateway Deployment

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: gateway
  namespace: mindbridge
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
        ports:
        - containerPort: 8090
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
        livenessProbe:
          httpGet:
            path: /api/health
            port: 8090
          initialDelaySeconds: 10
          periodSeconds: 30
```

### 3. Orchestrator Deployment

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: orchestrator
  namespace: mindbridge
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
        ports:
        - containerPort: 5009
        envFrom:
        - configMapRef:
            name: mindbridge-config
        - secretRef:
            name: mindbridge-secrets
        env:
        - name: SERVICE_NAME
          value: "orchestrator"
```

### 4. Counselor StatefulSet（3 个独立实例）

```yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: counselor
  namespace: mindbridge
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
        ports:
        - containerPort: 5010
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
        volumeMounts:
        - name: counselor-state
          mountPath: /app/.mindbridge
  volumeClaimTemplates:
  - metadata:
      name: counselor-state
    spec:
      accessModes: ["ReadWriteOnce"]
      resources:
        requests:
          storage: 1Gi
```

每个 Counselor 实例有独立的 PVC，保证状态隔离。Orchestrator 通过 `counselor-svc:5010/5012/5013` 访问。

### 5. Evaluator StatefulSet（3 个独立实例）

```yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: evaluator
  namespace: mindbridge
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
        ports:
        - containerPort: 5011
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
  volumeClaimTemplates:
  - metadata:
      name: evaluator-state
    spec:
      accessModes: ["ReadWriteOnce"]
      resources:
        requests:
          storage: 512Mi
```

### 6. Frontend Deployment

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: frontend
  namespace: mindbridge
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
        ports:
        - containerPort: 5173
        env:
        - name: SERVICE_NAME
          value: "frontend"
```

### 7. eBPF DaemonSet

```yaml
apiVersion: apps/v1
kind: DaemonSet
metadata:
  name: ebpf-monitor
  namespace: mindbridge
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

### 8. Services

```yaml
# Gateway Service
apiVersion: v1
kind: Service
metadata:
  name: gateway-svc
  namespace: mindbridge
spec:
  type: NodePort
  selector:
    app: gateway
  ports:
  - port: 8090
    targetPort: 8090
    nodePort: 30090

---
# Orchestrator Service
apiVersion: v1
kind: Service
metadata:
  name: orchestrator-svc
  namespace: mindbridge
spec:
  selector:
    app: orchestrator
  ports:
  - port: 5009
    targetPort: 5009

---
# Counselor Service (多端口)
apiVersion: v1
kind: Service
metadata:
  name: counselor-svc
  namespace: mindbridge
spec:
  selector:
    app: counselor
  ports:
  - name: port-5010
    port: 5010
    targetPort: 5010
  - name: port-5012
    port: 5012
    targetPort: 5012
  - name: port-5013
    port: 5013
    targetPort: 5013

---
# Evaluator Service (多端口)
apiVersion: v1
kind: Service
metadata:
  name: evaluator-svc
  namespace: mindbridge
spec:
  selector:
    app: evaluator
  ports:
  - name: port-5011
    port: 5011
    targetPort: 5011
  - name: port-5014
    port: 5014
    targetPort: 5014
  - name: port-5015
    port: 5015
    targetPort: 5015

---
# Frontend Service
apiVersion: v1
kind: Service
metadata:
  name: frontend-svc
  namespace: mindbridge
spec:
  type: NodePort
  selector:
    app: frontend
  ports:
  - port: 5173
    targetPort: 5173
    nodePort: 30173
```

### 9. ExternalName Services（宿主机存储）

```yaml
apiVersion: v1
kind: Service
metadata:
  name: mysql-ext
  namespace: mindbridge
spec:
  type: ExternalName
  externalName: host.docker.internal

---
apiVersion: v1
kind: Service
metadata:
  name: redis-ext
  namespace: mindbridge
spec:
  type: ExternalName
  externalName: host.docker.internal

---
apiVersion: v1
kind: Service
metadata:
  name: fastdfs-ext
  namespace: mindbridge
spec:
  type: ExternalName
  externalName: host.docker.internal
```

### 10. PVC

StatefulSet 使用 `volumeClaimTemplates` 自动创建 PVC，无需手动定义。

## 启动入口脚本

`start-service.sh` 根据 `SERVICE_NAME` 环境变量启动对应服务：

```bash
#!/bin/bash
case "$SERVICE_NAME" in
  gateway)
    exec ./mindbridge_gateway 8090
    ;;
  orchestrator)
    exec ./mindbridge_orchestrator 5009
    ;;
  counselor)
    exec ./mindbridge_counselor ${COUNSELOR_PORT:-5010}
    ;;
  evaluator)
    exec ./mindbridge_evaluator ${EVALUATOR_PORT:-5011}
    ;;
  frontend)
    exec python3 scripts/serve_demo_frontend.py --port 5173 --directory frontend/demo
    ;;
  ebpf-monitor)
    exec ./ebpf/mindbridge_ebpf_monitor -m 0 --trace-fs --trace-net --trace-resources --trace-tls
    ;;
esac
```

## 部署流程

### 前置条件

1. 安装 K3s：`curl -sfL https://get.k3s.io | sh -`
2. 安装 kubectl：`sudo apt install kubectl`
3. 启动宿主机存储服务（MySQL/Redis/FastDFS）

### 一键部署

```bash
# 构建镜像
docker build -t mindbridge:dev .

# 导入到 K3s
docker save mindbridge:dev | sudo k3s ctr images import -

# 创建 Secret
kubectl create secret generic mindbridge-secrets \
  --namespace mindbridge \
  --from-literal=DASHSCOPE_API_KEY=sk-xxx \
  --from-literal=MINDBRIDGE_SUDO_PASSWORD=' '

# 部署所有组件
kubectl apply -f k8s/
```

### 访问服务

- **前端**：`http://<node-ip>:30173/index.html`
- **Gateway API**：`http://<node-ip>:30090/api/health`

## 文件结构

```
mindbridge_project/
├── k8s/
│   ├── namespace.yaml
│   ├── configmap.yaml
│   ├── gateway.yaml
│   ├── orchestrator.yaml
│   ├── counselor.yaml
│   ├── evaluator.yaml
│   ├── frontend.yaml
│   ├── ebpf-daemonset.yaml
│   ├── services.yaml
│   ├── external-services.yaml
│   └── pvc.yaml
├── Dockerfile
├── start-service.sh
└── scripts/
    └── k8s-deploy.sh      # 一键部署脚本
```

## 验证清单

- [ ] 所有 Pod 状态为 Running
- [ ] Gateway /api/health 返回 200
- [ ] 前端页面可访问
- [ ] 发送消息能收到回复
- [ ] eBPF 事件正常采集
- [ ] 云存储功能正常（文件上传/下载）
