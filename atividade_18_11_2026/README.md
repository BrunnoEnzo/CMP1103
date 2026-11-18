# Atividade Prática - Streaming entre Dispositivos (18/11/2026)

Aplicação de transmissão e recepção de áudio via RTP/UDP entre dois computadores na rede local com compensação de jitter, controle de latência e salvamento do fluxo em MP3.

## Dependências

```bash
sudo apt update && sudo apt install -y libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly
```

## Compilação

```bash
gcc streaming_audio.c -o streaming_audio $(pkg-config --cflags --libs gstreamer-1.0) -Wall
```

## Execução entre Dois Computadores

### 1. Receptor (Dispositivo B - Iniciar primeiro)
Descubra o IPv4 da máquina (`ip a` ou `hostname -I`).

- **Via Argumento (recomendado para testes)**:
  ```bash
  # Teste 1 (latência 50 ms):
  ./streaming_audio -r 5000 50

  # Teste 2 (latência 200 ms):
  ./streaming_audio -r 5000 200
  ```
- **Via Modo Interativo**:
  ```bash
  ./streaming_audio
  # Escolha: 2 -> Porta: 5000 -> Latencia (ms): 50 ou 200
  ```
O receptor salva o áudio em `audio_recebido.mp3`, grava o log em `log_receptor_<latencia>ms.txt` e reproduz o som recebido ao final.

---

### 2. Transmissor (Dispositivo A)
Informe o IP do Dispositivo B e a porta correspondente.

- **Via Argumento**:
  ```bash
  ./streaming_audio -t <IP_DO_RECEPTOR> 5000 audio.mp3
  # Exemplo: ./streaming_audio -t 192.168.1.15 5000 audio.mp3
  ```
- **Via Modo Interativo**:
  ```bash
  ./streaming_audio
  # Escolha: 1 -> IP do receptor: 192.168.1.15 -> Porta: 5000
  ```
O transmissor exibe o cronômetro do streaming em tempo real até a conclusão.
