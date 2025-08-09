# Server REST Metriche C++20 con TLS

Un server REST completo scritto in C++20 per l'acquisizione di metriche da client Telegraf con autenticazione TLS. Il server supporta l'archiviazione in memoria delle metriche, query avanzate, statistiche, streaming real-time e compressione gzip.

## Caratteristiche Principali

- **Server HTTPS/TLS**: Supporto completo per TLS con autenticazione client opzionale
- **Validazione Certificati**: Verifica che i certificati client siano emessi dalla stessa CA
- **Compatibile con Telegraf**: Endpoint dedicato per ricevere metriche da Telegraf
- **API REST Complete**: Endpoint per inviare, interrogare e analizzare metriche
- **Storage In-Memory**: Archiviazione efficiente delle metriche con limite configurabile
- **Formato JSON**: Tutte le metriche sono gestite in formato JSON
- **Statistiche Real-time**: Calcolo di min, max, media, deviazione standard
- **Streaming Real-time**: Server-Sent Events (SSE) per ricevere metriche in tempo reale
- **Compressione Gzip**: Supporto automatico per richieste e risposte compresse
- **Thread-Safe**: Gestione concorrente sicura delle metriche
- **C++20 Moderno**: Utilizza le ultime funzionalità del linguaggio

## Requisiti

### Dipendenze di Sistema
- C++20 compiler (GCC 10+, Clang 10+, MSVC 2019+)
- CMake 3.16+
- OpenSSL 1.1.1+
- zlib 1.2.11+
- pthread (su sistemi Unix)

### Librerie C++ (scaricate automaticamente da CMake)
- [cpp-httplib](https://github.com/yhirose/cpp-httplib) - Server HTTP/HTTPS
- [nlohmann/json](https://github.com/nlohmann/json) - Parsing JSON

## Compilazione

### Metodo 1: Usando CMake (Raccomandato)

```bash
# Clona o crea la directory del progetto
mkdir metrics-server && cd metrics-server

# Crea i file del progetto (copia i file dal repository)
# ...

# Compila con CMake
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Metodo 2: Compilazione Manuale

```bash
# Installa le dipendenze su Ubuntu/Debian
sudo apt-get update
sudo apt-get install libssl-dev nlohmann-json3-dev zlib1g-dev

# Scarica cpp-httplib
wget https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h

# Compila
g++ -std=c++20 -O3 -o metrics_server metrics_server.cpp -lssl -lcrypto -pthread -lz
g++ -std=c++20 -O3 -o test_client test_client.cpp -lssl -lcrypto -pthread -lz
```

## Generazione Certificati TLS

Prima di avviare il server, è necessario generare i certificati TLS:

```bash
# Metodo 1: Usa lo script dedicato
chmod +x generate_certs.sh
./generate_certs.sh

# Metodo 2: Usa il server stesso
./metrics_server --generate-certs
```

Questo genera:
- `ca.crt` - Certificate Authority
- `server.crt`, `server.key` - Certificato server
- `client.crt`, `client.key` - Certificato client per Telegraf

## Avvio del Server

### Configurazione Base

```bash
# Avvia con configurazione predefinita
./metrics_server

# Con API key per autenticazione aggiuntiva
./metrics_server --api-key mysecretkey

# Porta e host personalizzati
./metrics_server --host 0.0.0.0 --port 9443

# Senza richiedere certificato client
./metrics_server --no-client-cert
```

### Opzioni Command Line

```
--host <address>      Indirizzo server (default: 0.0.0.0)
--port <port>         Porta server (default: 8443)
--cert <file>         File certificato server
--key <file>          File chiave privata server
--ca <file>           Certificato CA per autenticazione client
--api-key <key>       API key per autenticazione aggiuntiva
--no-client-cert      Disabilita requisito certificato client
--no-compression      Disabilita compressione gzip
--compression-level   Livello compressione 1-9 (default: 6)
--generate-certs      Genera certificati di esempio
--telegraf-config     Mostra configurazione Telegraf di esempio
--help                Mostra aiuto
```

## API REST Endpoints

### Health Check
```bash
GET /health

curl -k https://localhost:8443/health
```

Risposta:
```json
{
  "status": "healthy",
  "metrics_count": 1234,
  "timestamp": 1644345600
}
```

### Invio Metriche
```bash
POST /metrics

# Singola metrica
curl -k -X POST https://localhost:8443/metrics \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer mysecretkey" \
  -d '{
    "name": "cpu_usage",
    "tags": {"host": "server1", "region": "eu-south"},
    "fields": {"usage": 45.5, "temperature": 65.0}
  }'

# Multiple metriche
curl -k -X POST https://localhost:8443/metrics \
  -H "Content-Type: application/json" \
  -d '[
    {"name": "cpu", "tags": {"host": "srv1"}, "fields": {"usage": 45.5}},
    {"name": "memory", "tags": {"host": "srv1"}, "fields": {"used": 4096}}
  ]'
```

### Query Metriche
```bash
GET /metrics?name=<metric_name>&tag.<key>=<value>&limit=<n>

# Tutte le metriche
curl -k https://localhost:8443/metrics

# Filtra per nome
curl -k https://localhost:8443/metrics?name=cpu_usage

# Filtra per tags
curl -k https://localhost:8443/metrics?name=cpu_usage&tag.host=server1

# Limita risultati
curl -k https://localhost:8443/metrics?limit=100
```

### Statistiche
```bash
GET /metrics/<metric_name>/stats?field=<field_name>

curl -k https://localhost:8443/metrics/cpu_usage/stats?field=usage
```

Risposta:
```json
{
  "count": 100,
  "min": 10.5,
  "max": 95.2,
  "mean": 45.6,
  "median": 44.8,
  "stddev": 15.3,
  "sum": 4560.0
}
```

### Endpoint Telegraf
```bash
POST /telegraf

# Formato specifico per Telegraf
```

### Streaming Real-time (Server-Sent Events)
```bash
GET /stream?metrics=<metric1,metric2>&tag.<key>=<value>

# Esempio: ricevi solo metriche CPU e memory da server1
curl -k -H "Authorization: Bearer mysecretkey" \
     -H "Accept: text/event-stream" \
     "https://localhost:8443/stream?metrics=cpu,memory&tag.host=server1"
```

Formato eventi SSE:
```
event: metric
data: {"name":"cpu","tags":{"host":"server1"},"fields":{"usage":45.5},"timestamp":1644345600000}

:heartbeat
```

### Long Polling (WebSocket alternativo)
```bash
GET /ws/metrics?client_id=<id>

# Ottieni nuove metriche dall'ultima richiesta
curl -k "https://localhost:8443/ws/metrics?client_id=myclient"
```

## Compressione Gzip

Il server supporta automaticamente la compressione gzip per richieste e risposte:

```bash
# Richiesta con compressione
curl -k -X POST https://localhost:8443/metrics \
     -H "Content-Type: application/json" \
     -H "Content-Encoding: gzip" \
     -H "Authorization: Bearer mysecretkey" \
     --data-binary @metrics.json.gz

# Richiesta di risposta compressa
curl -k https://localhost:8443/metrics \
     -H "Accept-Encoding: gzip" \
     -H "Authorization: Bearer mysecretkey" \
     --compressed
```

## Configurazione Telegraf

Crea un file `telegraf.conf`:

```toml
[[outputs.http]]
  url = "https://localhost:8443/telegraf"
  method = "POST"
  data_format = "json"
  
  # Certificati TLS
  tls_ca = "/path/to/ca.crt"
  tls_cert = "/path/to/client.crt"
  tls_key = "/path/to/client.key"
  
  # API Key (se configurata)
  headers = {"Authorization" = "Bearer mysecretkey"}
  
  # Configurazione batch
  metric_batch_size = 1000
  metric_buffer_limit = 10000
  timeout = "5s"

# Input plugins
[[inputs.cpu]]
  percpu = true
  totalcpu = true

[[inputs.mem]]

[[inputs.disk]]
  ignore_fs = ["tmpfs", "devtmpfs"]
```

Avvia Telegraf:
```bash
telegraf --config telegraf.conf
```

## Client di Test

Il progetto include un client di test completo:

```bash
# Test di base
./test_client --host localhost --port 8443 --api-key mysecretkey --test

# Simulazione con invio continuo di metriche
./test_client --simulate 60 --interval 1000

# Test formato Telegraf
./test_client --telegraf

# Test streaming real-time
./test_client --streaming 30

# Test compressione
./test_client --compression

# Con certificati client
./test_client --ca ca.crt --cert client.crt --key client.key --test
```

## Formato Dati Metriche

### Struttura Metrica
```json
{
  "name": "string",              // Nome della metrica (required)
  "tags": {                      // Tags per categorizzazione (optional)
    "key1": "value1",
    "key2": "value2"
  },
  "fields": {                    // Valori numerici (required)
    "field1": 123.45,
    "field2": 67.89
  },
  "timestamp": 1644345600000     // Unix timestamp in millisecondi (optional)
}
```

### Esempi di Metriche

**System Metrics:**
```json
{
  "name": "system",
  "tags": {
    "host": "webserver-01",
    "datacenter": "milan-1"
  },
  "fields": {
    "cpu_usage": 45.5,
    "memory_used": 4096,
    "disk_usage": 78.2
  }
}
```

**Application Metrics:**
```json
{
  "name": "http_requests",
  "tags": {
    "service": "api",
    "endpoint": "/users",
    "method": "GET"
  },
  "fields": {
    "count": 1523,
    "response_time_ms": 45.2,
    "error_rate": 0.02
  }
}
```

## Sicurezza

### TLS/SSL
- Il server supporta TLS 1.2+
- Autenticazione client tramite certificati (opzionale)
- Validazione che i certificati client siano emessi dalla stessa CA del server
- Profondità di verifica della catena di certificati configurabile

### Validazione Certificati
Il server verifica automaticamente che:
1. Il certificato client sia valido e non scaduto
2. Il certificato sia stato firmato dalla CA configurata
3. La catena di certificazione sia completa e valida
4. Il certificato non sia stato revocato (se CRL configurata)

### Autenticazione
- **Certificati Client**: Richiesti di default (disabilitabile)
- **API Key**: Header `Authorization: Bearer <key>` opzionale
- **Doppia Autenticazione**: Possibile combinare certificati + API key

### Best Practices
1. Usa sempre certificati validi in produzione (non self-signed)
2. Mantieni le chiavi private sicure con permessi 600
3. Ruota regolarmente certificati e API keys
4. Limita l'accesso alla porta del server tramite firewall
5. Monitora i log per accessi non autorizzati

## Performance e Limiti

- **Storage**: Massimo 10.000 metriche in memoria (configurabile)
- **Batch Size**: Supporta batch fino a 1000 metriche per richiesta
- **Concorrenza**: Thread pool con 16 thread per gestire richieste
- **Timeout**: 5 secondi per connessione/lettura/scrittura
- **Compressione**: Riduce il traffico di rete del 60-90% per payload JSON
- **Streaming**: Supporta centinaia di client SSE simultanei

## Troubleshooting

### Il server non si avvia
```bash
# Verifica i certificati
openssl x509 -in server.crt -text -noout

# Verifica che la porta sia libera
sudo netstat -tlnp | grep 8443
```

### Errori di connessione TLS
```bash
# Test connessione con openssl
openssl s_client -connect localhost:8443 -cert client.crt -key client.key

# Test senza verifica certificato
curl -k https://localhost:8443/health
```

### Telegraf non invia metriche
1. Verifica i log di Telegraf: `telegraf --debug`
2. Controlla i certificati nel file di configurazione
3. Verifica connettività: `telnet localhost 8443`

## Esempi Avanzati

### Monitoraggio Multi-Host
```bash
# Script per inviare metriche da più host
for host in server{1..10}; do
  curl -k -X POST https://metrics.example.com:8443/metrics \
    -H "Content-Type: application/json" \
    -H "Accept-Encoding: gzip" \
    -d "[
      {\"name\": \"cpu\", \"tags\": {\"host\": \"$host\"}, \"fields\": {\"usage\": $((RANDOM % 100))}},
      {\"name\": \"memory\", \"tags\": {\"host\": \"$host\"}, \"fields\": {\"used\": $((RANDOM % 8192))}}
    ]"
done
```

### Query con Aggregazione
```bash
# Ottieni statistiche CPU per tutti gli host
for host in server{1..10}; do
  echo "Stats for $host:"
  curl -sk "https://localhost:8443/metrics/cpu/stats?field=usage&tag.host=$host"
  echo
done
```

### Streaming Real-time con Filtri
```bash
# Monitora solo metriche critiche in tempo reale
curl -k -N -H "Authorization: Bearer mysecretkey" \
     "https://localhost:8443/stream?metrics=cpu,memory,disk&tag.alert=critical" \
     | while IFS= read -r line; do
       if [[ $line == data:* ]]; then
         echo "Alert: ${line:6}" | jq .
       fi
     done
```

### Dashboard Real-time con Node.js
```javascript
const EventSource = require('eventsource');

const es = new EventSource('https://localhost:8443/stream', {
  headers: {
    'Authorization': 'Bearer mysecretkey'
  },
  rejectUnauthorized: false // Solo per certificati self-signed
});

es.addEventListener('metric', (event) => {
  const metric = JSON.parse(event.data);
  console.log(`[${metric.name}] ${metric.tags.host}: ${JSON.stringify(metric.fields)}`);
});
```

## Sviluppo e Contributi

### Struttura del Codice
- `MetricsRESTServer`: Classe principale del server
- `MetricsStorage`: Storage thread-safe per le metriche
- `Metric`: Struttura dati per rappresentare una metrica

### Estensioni Possibili
1. **Persistenza**: Aggiungere backend database (InfluxDB, PostgreSQL)
2. **Autenticazione**: Supporto OAuth2, JWT
3. **WebSocket Nativo**: Implementazione completa WebSocket con libreria dedicata
4. **Prometheus**: Export in formato Prometheus
5. **Clustering**: Supporto per deployment multi-nodo con sincronizzazione

## Licenza

Questo progetto è fornito come esempio educativo. Sentiti libero di usarlo e modificarlo secondo le tue necessità.

## Supporto

Per problemi o domande:
1. Controlla i log del server per messaggi di errore
2. Verifica la configurazione TLS e i certificati
3. Assicurati che tutte le dipendenze siano installate correttamente

---

Sviluppato con ❤️ in C++20 per gestione metriche ad alte prestazioni.