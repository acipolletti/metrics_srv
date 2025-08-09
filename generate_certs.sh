#!/bin/bash
# Genera CA
openssl genrsa -out ca.key 2048
openssl req -new -x509 -days 365 -key ca.key -out ca.crt -subj "/C=IT/ST=Lombardy/L=Milan/O=MetricsServer/CN=MetricsCA"

# Genera certificato server
openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr -subj "/C=IT/ST=Lombardy/L=Milan/O=MetricsServer/CN=localhost"
openssl x509 -req -days 365 -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server.crt

# Genera certificato client per Telegraf
openssl genrsa -out client.key 2048
openssl req -new -key client.key -out client.csr -subj "/C=IT/ST=Lombardy/L=Milan/O=MetricsServer/CN=telegraf-client"
openssl x509 -req -days 365 -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out client.crt

echo "Certificates generated successfully!"
