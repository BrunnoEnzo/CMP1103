# Streaming de Áudio entre Dispositivos

Aplicação em C/GStreamer para streaming de áudio RTP/UDP em tempo real na rede local com compensação de jitter e controle de latência.

## Compilação

```bash
gcc streaming_audio.c -o streaming_audio $(pkg-config --cflags --libs gstreamer-1.0) -Wall
```

## Execução

### 1. Receptor (executar primeiro)
```bash
./streaming_audio -r 5000 200
```
*O programa exibe automaticamente na tela o IP da interface de rede local para conectar. Latências testadas: 50 e 200 ms.*

### 2. Transmissor
```bash
./streaming_audio -t <IP_DO_RECEPTOR> 5000 audio.mp3
```
*Exibe o progresso em megabytes (MB) transmitidos em tempo real.*

---
*Arquitetura completa documentada em `Diagrama Atividade 11.png`.*
