# Metrics Server per Telegraf

Server C++ per ricevere metriche da Telegraf in formato JSON.

## Caratteristiche

- Supporto HTTP e HTTPS
- Gestione sicura del parsing JSON (nessun crash per chiavi mancanti)
- Logging delle metriche su file
- Endpoint di health check e statistiche
- Gestione errori robusta

## Compilazione

```bash
chmod +x build.sh
./build.sh
```

## Dipendenze

- CMake >= 3.14
- C++17 compiler
- OpenSSL
- Le librerie cpp-httplib e nlohmann/json vengono scaricate automaticamente

## Uso

### Avvio del server

```bash
# Solo HTTPS (porta 8443)
./build/metrics_server --https-only

# Solo HTTP (porta 8080)
./build/metrics_server --http-only

# Automatico (prova HTTPS, fallback a HTTP)
./build/metrics_server
```

### Configurazione Telegraf

```toml
[[outputs.http]]
  # Per HTTPS
  url = "https://192.168.1.61:8443/metrics"
  insecure_skip_verify = true
  
  # Per HTTP
  # url = "http://192.168.1.61:8080/metrics"
  
  timeout = "5s"
  method = "POST"
  data_format = "json"
```

### Endpoint disponibili

- `POST /metrics` - Riceve metriche da Telegraf
- `GET /health` - Health check
- `GET /metrics/stats` - Statistiche sulle metriche ricevute

## Risoluzione problemi

### Errore "Assertion failed"

Il codice originale crashava perché accedeva a chiavi JSON senza verificarne l'esistenza.
Questa versione usa:
- `.contains()` per verificare l'esistenza delle chiavi
- `.value()` con valori di default
- Try-catch per gestire errori di parsing

### Best practices per JSON sicuro

```cpp
// ❌ NON FARE - Può crashare se la chiave non esiste
string name = data["name"];

// ✅ FAI COSÌ - Sicuro con default
string name = data.value("name", "default");

// ✅ O COSÌ - Verifica prima
if (data.contains("name")) {
    string name = data["name"];
}
```

## File di log

Le metriche vengono salvate in `metrics.jsonl` (formato JSON Lines).

## Debug

Per vedere tutti i dati ricevuti, il server stampa su stdout:
- Raw data ricevuti
- Prime 3 metriche per ogni batch
- Eventuali errori di parsing

