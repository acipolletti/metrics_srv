#!/bin/bash
# test_with_curl.sh - Test completo del server metriche usando cURL

# Colori per output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Configurazione
SERVER_URL="${SERVER_URL:-https://localhost:8443}"
API_KEY="${API_KEY:-mysecretkey}"
CA_CERT="${CA_CERT:-ca.crt}"
CLIENT_CERT="${CLIENT_CERT:-client.crt}"
CLIENT_KEY="${CLIENT_KEY:-client.key}"

echo -e "${BLUE}=== Test Server Metriche con cURL ===${NC}"
echo

# Funzione per test con output formattato
run_test() {
    local test_name=$1
    local curl_cmd=$2
    
    echo -e "${YELLOW}Test: $test_name${NC}"
    echo -e "${BLUE}Comando:${NC} $curl_cmd"
    
    # Esegui comando e cattura output e exit code
    output=$(eval "$curl_cmd" 2>&1)
    exit_code=$?
    
    if [ $exit_code -eq 0 ]; then
        echo -e "${GREEN}✓ Test passato${NC}"
        if [ ! -z "$output" ]; then
            echo "$output" | jq . 2>/dev/null || echo "$output"
        fi
    else
        echo -e "${RED}✗ Test fallito (exit code: $exit_code)${NC}"
        echo "$output"
    fi
    echo
    return $exit_code
}

# 1. Test Health Check
run_test "Health Check" \
    "curl -s -k --cert $CLIENT_CERT --key $CLIENT_KEY --cacert $CA_CERT \
     -H 'Authorization: Bearer $API_KEY' \
     $SERVER_URL/health"

# 2. Test Invio Metrica Singola
run_test "Invio Metrica Singola" \
    "curl -s -k --cert $CLIENT_CERT --key $CLIENT_KEY --cacert $CA_CERT \
     -X POST $SERVER_URL/metrics \
     -H 'Authorization: Bearer $API_KEY' \
     -H 'Content-Type: application/json' \
     -d '{\"name\":\"test_metric\",\"tags\":{\"host\":\"test-host\"},\"fields\":{\"value\":42.5}}'"

# 3. Test Invio Batch di Metriche
run_test "Invio Batch Metriche" \
    "curl -s -k --cert $CLIENT_CERT --key $CLIENT_KEY --cacert $CA_CERT \
     -X POST $SERVER_URL/metrics \
     -H 'Authorization: Bearer $API_KEY' \
     -H 'Content-Type: application/json' \
     -d '[
       {\"name\":\"cpu\",\"tags\":{\"host\":\"server1\"},\"fields\":{\"usage\":45.5}},
       {\"name\":\"memory\",\"tags\":{\"host\":\"server1\"},\"fields\":{\"used\":4096,\"total\":8192}},
       {\"name\":\"disk\",\"tags\":{\"host\":\"server1\",\"device\":\"/dev/sda1\"},\"fields\":{\"usage\":78.2}}
     ]'"

# 4. Test Query Metriche
run_test "Query Tutte le Metriche" \
    "curl -s -k --cert $CLIENT_CERT --key $CLIENT_KEY --cacert $CA_CERT \
     -H 'Authorization: Bearer $API_KEY' \
     $SERVER_URL/metrics?limit=5"

# 5. Test Query con Filtri
run_test "Query con Filtri" \
    "curl -s -k --cert $CLIENT_CERT --key $CLIENT_KEY --cacert $CA_CERT \
     -H 'Authorization: Bearer $API_KEY' \
     '$SERVER_URL/metrics?name=cpu&tag.host=server1'"

# 6. Test Statistiche
run_test "Statistiche CPU" \
    "curl -s -k --cert $CLIENT_CERT --key $CLIENT_KEY --cacert $CA_CERT \
     -H 'Authorization: Bearer $API_KEY' \
     '$SERVER_URL/metrics/cpu/stats?field=usage'"

# 7. Test Compressione Gzip
echo -e "${YELLOW}Test: Compressione Gzip${NC}"

# Crea payload grande
large_payload='['
for i in {1..100}; do
    large_payload+="{\"name\":\"metric_$i\",\"tags\":{\"host\":\"host_$i\"},\"fields\":{\"value\":$RANDOM}},"
done
large_payload="${large_payload%,}]"

# Salva su file temporaneo
echo "$large_payload" > /tmp/large_metrics.json
original_size=$(stat -c%s /tmp/large_metrics.json 2>/dev/null || stat -f%z /tmp/large_metrics.json)

echo "Dimensione originale: $original_size bytes"

# Invia con compressione
run_test "Invio con Accept-Encoding: gzip" \
    "curl -s -k --cert $CLIENT_CERT --key $CLIENT_KEY --cacert $CA_CERT \
     -X POST $SERVER_URL/metrics \
     -H 'Authorization: Bearer $API_KEY' \
     -H 'Content-Type: application/json' \
     -H 'Accept-Encoding: gzip' \
     --data-binary @/tmp/large_metrics.json \
     --compressed -w '\nResponse size: %{size_download} bytes'"

rm -f /tmp/large_metrics.json

# 8. Test Streaming SSE
echo -e "${YELLOW}Test: Streaming SSE (10 secondi)${NC}"
echo "Connessione allo stream..."

# Avvia streaming in background
(
    curl -N -s -k --cert $CLIENT_CERT --key $CLIENT_KEY --cacert $CA_CERT \
         -H 'Authorization: Bearer $API_KEY' \
         -H 'Accept: text/event-stream' \
         "$SERVER_URL/stream?metrics=test_metric,cpu&tag.host=test-host" \
         2>/dev/null | while IFS= read -r line; do
        if [[ $line == data:* ]]; then
            echo -e "${GREEN}📊 Ricevuto:${NC} ${line:5}" | jq . 2>/dev/null || echo "${line:5}"
        elif [[ $line == :heartbeat* ]]; then
            echo -e "${BLUE}💓 Heartbeat${NC}"
        fi
    done
) &

STREAM_PID=$!

# Invia alcune metriche mentre lo streaming è attivo
sleep 2
for i in {1..3}; do
    echo -e "${YELLOW}Invio metrica streaming $i...${NC}"
    curl -s -k --cert $CLIENT_CERT --key $CLIENT_KEY --cacert $CA_CERT \
         -X POST $SERVER_URL/metrics \
         -H 'Authorization: Bearer $API_KEY' \
         -H 'Content-Type: application/json' \
         -d "{\"name\":\"test_metric\",\"tags\":{\"host\":\"test-host\"},\"fields\":{\"streaming_value\":$((i*10))}}" \
         >/dev/null 2>&1
    sleep 2
done

# Termina streaming
sleep 3
kill $STREAM_PID 2>/dev/null
echo -e "${GREEN}✓ Test streaming completato${NC}"
echo

# 9. Test Telegraf Format
run_test "Formato Telegraf" \
    "curl -s -k --cert $CLIENT_CERT --key $CLIENT_KEY --cacert $CA_CERT \
     -X POST $SERVER_URL/telegraf \
     -H 'Authorization: Bearer $API_KEY' \
     -H 'Content-Type: application/json' \
     -d '{\"metrics\":[{\"name\":\"cpu\",\"tags\":{\"host\":\"telegraf-test\"},\"fields\":{\"usage\":55.5},\"timestamp\":$(date +%s)}]}' \
     -w '\nStatus: %{http_code}'"

# 10. Test Validazione Certificati
echo -e "${YELLOW}Test: Validazione Certificati${NC}"

# Test senza certificato client (dovrebbe fallire se require_client_cert è true)
echo "Test connessione senza certificato client:"
curl -s -k -H "Authorization: Bearer $API_KEY" $SERVER_URL/health >/dev/null 2>&1
if [ $? -ne 0 ]; then
    echo -e "${GREEN}✓ Server richiede correttamente certificato client${NC}"
else
    echo -e "${YELLOW}⚠ Server accetta connessioni senza certificato client${NC}"
fi

# Test con certificato non valido
if [ -f "invalid.crt" ] && [ -f "invalid.key" ]; then
    echo "Test con certificato non valido:"
    curl -s -k --cert invalid.crt --key invalid.key --cacert $CA_CERT \
         -H "Authorization: Bearer $API_KEY" $SERVER_URL/health >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo -e "${GREEN}✓ Server rifiuta correttamente certificati non validi${NC}"
    else
        echo -e "${RED}✗ Server accetta certificati non validi${NC}"
    fi
fi

echo
echo -e "${BLUE}=== Riepilogo Test ===${NC}"
echo "Server URL: $SERVER_URL"
echo "Certificati utilizzati:"
echo "  - CA: $CA_CERT"
echo "  - Client: $CLIENT_CERT"
echo "  - Key: $CLIENT_KEY"
echo
echo -e "${GREEN}Test completati!${NC}"
echo
echo -e "${YELLOW}Suggerimenti:${NC}"
echo "- Per test senza certificati client: export CLIENT_CERT='' CLIENT_KEY=''"
echo "- Per cambiare server: export SERVER_URL='https://altro-server:8443'"
echo "- Per vedere i dettagli SSL: aggiungi -v ai comandi curl"