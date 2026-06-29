FROM gcr.io/oss-fuzz-base/base-builder:v1
RUN apt-get update && apt-get install -y python3
COPY . $SRC
WORKDIR $SRC
