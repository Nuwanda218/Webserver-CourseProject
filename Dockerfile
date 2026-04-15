# base image from gradescope
FROM gradescope/auto-builds:ubuntu-18.04

# 替换 apt 源为阿里云镜像（加速软件包下载）
RUN sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
    sed -i 's/security.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list

# 更新并安装所需软件包
RUN apt-get update && \
    apt-get -y install gcc flex bison build-essential siege apache2-utils libssl-dev && \
    # change ApacheBench request HTTP version to 1.1
    perl -pi -e 's/HTTP\/1.0/HTTP\/1.1/g' /usr/bin/ab

WORKDIR /home