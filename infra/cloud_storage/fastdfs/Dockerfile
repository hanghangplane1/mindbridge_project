FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    ca-certificates curl gcc g++ make wget lsof procps \
    libtool automake autoconf libpcre3-dev libssl-dev zlib1g-dev openssl \
    && rm -rf /var/lib/apt/lists/*

COPY deps/libfastcommon-V1.0.43.tar.gz /tmp/
COPY deps/fastdfs-V6.06.tar.gz /tmp/
COPY deps/fastdfs-nginx-module-V1.22.tar.gz /tmp/

RUN cd /tmp && \
    tar xzf libfastcommon-V1.0.43.tar.gz && \
    cd libfastcommon-1.0.43 && \
    ./make.sh && ./make.sh install && \
    ldconfig && \
    rm -rf /tmp/libfastcommon-1.0.43 /tmp/libfastcommon-V1.0.43.tar.gz

RUN cd /tmp && \
    tar xzf fastdfs-V6.06.tar.gz && \
    cd fastdfs-6.06 && \
    ./make.sh && ./make.sh install && \
    ldconfig && \
    mkdir -p /etc/fdfs && \
    cp conf/http.conf /etc/fdfs/http.conf && \
    cp conf/mime.types /etc/fdfs/mime.types && \
    rm -rf /tmp/fastdfs-6.06 /tmp/fastdfs-V6.06.tar.gz

RUN cd /tmp && \
    tar xzf fastdfs-nginx-module-V1.22.tar.gz && \
    wget -q http://nginx.org/download/nginx-1.20.2.tar.gz && \
    tar xzf nginx-1.20.2.tar.gz && \
    cd nginx-1.20.2 && \
    ./configure --prefix=/usr/local/nginx \
      --add-module=/tmp/fastdfs-nginx-module-1.22/src \
      --with-http_ssl_module && \
    make && make install && \
    rm -rf /tmp/nginx-1.20.2 /tmp/nginx-1.20.2.tar.gz /tmp/fastdfs-nginx-module-1.22 /tmp/fastdfs-nginx-module-V1.22.tar.gz

RUN mkdir -p /fastdfs_data_and_log/tracker \
    /fastdfs_data_and_log/storage \
    /fastdfs_data_and_log/client \
    /etc/fdfs

COPY fastdfs/tracker.conf /etc/fdfs/tracker.conf
COPY fastdfs/storage.conf /etc/fdfs/storage.conf
COPY fastdfs/client.conf /etc/fdfs/client.conf
COPY fastdfs/mod_fastdfs.conf /etc/fdfs/mod_fastdfs.conf
COPY fastdfs/nginx.conf /usr/local/nginx/conf/nginx.conf
COPY fastdfs/start.sh /app/start.sh
RUN chmod +x /app/start.sh

EXPOSE 80 443 22122 23000

CMD ["/app/start.sh"]
