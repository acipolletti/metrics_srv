# 🚀 Guida Rapida - Server Metriche

## Compilazione Rapida (Risolve Errori cpp-httplib)

### Opzione 1: Makefile (Consigliato)
```bash
# Installa dipendenze
sudo apt-get update
sudo apt-get install -y g++ libssl-dev zlib1g-dev nlohmann-json3-dev make

# Scarica httplib.h più recente
wget -O httplib.h https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h

# Compila tutto
make clean
make all

# Genera certificati
make certs

# Avvia server
make run
```

### Opzione 2: Script Build
```bash
chmod +x build.sh
./build.sh
```

### Opzione 3: Compilazione Manuale
```bash
# Server (con tutte le define necessarie)
g++ -std=c++20 -O3 \
    -DCPPHTTPLIB_OPENSSL_SUPPORT \
    -DCPPHTTPLIB_ZLIB_SUPPORT \
    -DCPPHTTPLIB_THREAD_POOL_COUNT=16 \
    -o metrics_server metrics_server.cpp \
    -lssl -lcrypto -pthread -lz

# Client di test (versione semplificata)
g++ -std=c++20 -O3 \
    -DCPPHTTPLIB_OPENSSL_SUPPORT \
    -DCPPHTTPLIB_ZLIB_SUPPORT \
    -o test_client test_client.cpp \
    -lssl -lcrypto -pthread -lz

# Client SSL alternativo (per test certificati)
g++ -std=c++20 -o simple_ssl_client simple_ssl_client.cpp -lssl -lcrypto
```

## Test Rapidi

### Test senza Certificati Client
```bash
# Avvia server senza richiedere certificati client
./metrics_server --no-client-cert --api-key test123

# Test con curl
curl -k https://localhost:8443/health -H "Authorization: Bearer test123"
```

### Test con Certificati Client
```bash
# Usa script di test con cURL (più affidabile)
chmod +x test_with_curl.sh
API_KEY=test123 ./test_with_curl.sh

# Oppure usa il client SSL dedicato
./simple_ssl_client --api-key test123
```

### Test Streaming
```bash
# Streaming con cURL
curl -N -k https://localhost:8443/stream \
     -H "Authorization: Bearer test123"

# Dashboard web
# Apri streaming-dashboard.html nel browser
```

## Risoluzione Problemi Comuni

### Errore: 'set_ca_cert_path' not found
- **Causa**: Versione vecchia di cpp-httplib
- **Soluzione**: Scarica l'ultima versione di httplib.h

### Errore: 'set_client_cert_path' not found  
- **Causa**: cpp-httplib potrebbe non supportare certificati client
- **Soluzione**: Usa `test_with_curl.sh` o `simple_ssl_client` per test con certificati

### Errore: Compressione non funziona
- **Causa**: Manca la define CPPHTTPLIB_ZLIB_SUPPORT
- **Soluzione**: Ricompila con `-DCPPHTTPLIB_ZLIB_SUPPORT`

### Server rifiuta connessioni
- **Causa**: Certificati client richiesti ma non forniti
- **Soluzione**: Avvia con `--no-client-cert` o fornisci certificati validi

## Comandi Utili

```bash
# Verifica certificati
openssl x509 -in server.crt -text -noout

# Test connessione SSL
openssl s_client -connect localhost:8443 -showcerts

# Monitor metriche in tempo reale
watch -n 1 'curl -sk https://localhost:8443/metrics?limit=5 | jq .'

# Invia metriche di test continue
while true; do 
    curl -sk -X POST https://localhost:8443/metrics \
         -H "Content-Type: application/json" \
         -d "[{\"name\":\"test\",\"fields\":{\"value\":$RANDOM}}]"
    sleep 1
done
```

## Link Utili
- [cpp-httplib](https://github.com/yhirose/cpp-httplib)
- [nlohmann/json](https://github.com/nlohmann/json)
- [OpenSSL Docs](https://www.openssl.org/docs/)