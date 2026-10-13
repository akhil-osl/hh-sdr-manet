FROM ubuntu:22.04 AS build

COPY ca-certificates.crt /etc/ssl/certs/ca-certificates.crt

ARG OONF_REPO=https://github.com/OLSR/OONF.git
ARG OONF_COMMIT=v0.15.1

RUN sed -i \
    -e 's|http://archive.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g' \
    -e 's|http://security.ubuntu.com/ubuntu|https://security.ubuntu.com/ubuntu|g' \
    /etc/apt/sources.list \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates git cmake make gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone "$OONF_REPO" /oonf \
    && git -C /oonf checkout "$OONF_COMMIT" \
    && git -C /oonf describe --always --long > /oonf/VERSION \
    && git -C /oonf rev-parse HEAD >> /oonf/VERSION

RUN cmake -S /oonf -B /oonf/build \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS=-fcommon \
    && cmake --build /oonf/build -j"$(nproc)" --target olsrd2_static


FROM ubuntu:22.04

COPY ca-certificates.crt /etc/ssl/certs/ca-certificates.crt

RUN sed -i \
    -e 's|http://archive.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g' \
    -e 's|http://security.ubuntu.com/ubuntu|https://security.ubuntu.com/ubuntu|g' \
    /etc/apt/sources.list \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        iproute2 nftables iputils-ping traceroute iperf3 tcpdump netcat-openbsd \
        gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /oonf/build/olsrd2_static /usr/local/sbin/olsrd2
COPY --from=build /oonf/VERSION /etc/oonf-version
COPY olsrd2.conf /etc/olsrd2/olsrd2.conf

CMD ["sleep", "infinity"]
