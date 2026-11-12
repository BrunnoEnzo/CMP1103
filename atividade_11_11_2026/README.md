# Atividade 3 - 11/11/2026

Aplicação de streaming de áudio sobre RTP/UDP com compensação de jitter (`rtpjitterbuffer`), diagnóstico de perdas, gravação do arquivo recebido e reprodução sonora final.

## Compilação

```bash
gcc streaming_audio.c -o streaming_audio $(pkg-config --cflags --libs gstreamer-1.0) -Wall
```

## Execução

```bash
./streaming_audio
```

### Configuração

1. **Receptor (iniciar primeiro)**:
   - Digite `2`
   - Porta (padrão: `5000`)
   - Latência do Jitter Buffer em ms (padrão: `200`)
   - O áudio será salvo em `audio_recebido.mp3`, o relatório em `log_receptor.txt` e reproduzido ao final.

2. **Transmissor**:
   - Digite `1`
   - IP do Receptor (ex: `192.168.1.15` ou `127.0.0.1` para teste local)
   - Porta (mesma usada no receptor)
   - Transmite o arquivo `audio.mp3`.
