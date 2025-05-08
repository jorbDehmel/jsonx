FROM ubuntu:latest
RUN apt-get update && apt-get install -y quickjs nodejs npm
WORKDIR /host
RUN npm install --save-dev typescript
CMD bash
