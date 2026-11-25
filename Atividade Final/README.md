# Atividade Final - Streaming Multimídia de Áudio com Interface Gráfica (GTK 3)

Evolução do sistema de streaming multimídia em C e GStreamer desenvolvido ao longo da disciplina, implementando interfaces gráficas modernas em **GTK 3** para o **Servidor (Transmissor)** e **Cliente (Receptor)**. O sistema realiza transmissão e reprodução contínua de áudio em tempo real via **RTP L16 sobre UDP**, com controle dinâmico de jitter buffer, telemetria ao vivo e suporte tanto para `127.0.0.1` (localhost) quanto para comunicação entre dispositivos distintos na rede local.

---

## Estrutura do Projeto

- **`servidor.c`**: Aplicação gráfica GTK 3 do transmissor de áudio:
  - Configuração do **IP de destino** e **Porta UDP**.
  - Seleção do arquivo de áudio (`audio.mp3` padrão ou navegador via `GtkFileChooserDialog`).
  - Parâmetros de qualidade configuráveis: **Taxa de amostragem** (44.1 kHz, 48 kHz, 22.05 kHz) e **Canais** (Estéreo / Mono).
  - **Barra de progresso** em tempo real com volume transmitido em Megabytes (`MB / Total MB`) e porcentagem (`%`).
  - Pipeline: `uridecodebin -> audioconvert -> audioresample -> capsfilter (audio/x-raw S16BE) -> rtpL16pay -> udpsink`.
- **`cliente.c`**: Aplicação gráfica GTK 3 do receptor de áudio:
  - Exibição automática do **IP local** da máquina receptora para facilitar o preenchimento no servidor.
  - Seleção da **Porta UDP** de escuta e **Latência do Jitter Buffer** (50 ms, 100 ms, 200 ms, 400 ms).
  - Parâmetros de recepção compatíveis (taxa de amostragem e canais).
  - **Painel de Telemetria ao Vivo**: monitoramento em tempo real de pacotes recebidos (`num-pushed`), perdidos (`num-lost`), atrasados (`num-late`), duplicados e jitter médio calculado (`avg-jitter`).
  - Barra pulsante de atividade sonora e saída direta de som no alto-falante (`alsasink` / `pulsesink` / `autoaudiosink`).
  - Geração automática de relatório consolidado em arquivo (`log_receptor_<latencia>ms.txt` e `log_receptor.txt`) ao finalizar ou interromper.
- **`streaming_audio.c`**: Aplicação em modo texto (CLI) desenvolvida nas aulas anteriores, mantida para compatibilidade e testes via terminal.
- **`audio.mp3`**: Arquivo de áudio para testes práticos de streaming.
- **`Makefile`**: Script de compilação automatizada com flags rigorosas (`-Wall -Wextra -O2`).
- **`Atividade_Final_Streaming_GStreamer.pdf`**: Roteiro oficial da atividade.

---

## Dependências

### Ubuntu / Debian / Linux Mint:
```bash
sudo apt update
sudo apt install -y build-essential pkg-config libgtk-3-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-alsa gstreamer1.0-pulseaudio
```

### Fedora:
```bash
sudo dnf install -y gcc make pkgconf-pkg-config gtk3-devel gstreamer1-devel gstreamer1-plugins-base-devel gstreamer1-plugins-good gstreamer1-plugins-bad-free gstreamer1-plugins-ugly-free gstreamer1-libav
```

---

## Compilação

Para compilar todos os programas (`servidor`, `cliente` e `streaming_audio`):
```bash
make
```

Para limpar os binários gerados:
```bash
make clean
```

---

## Instruções de Execução

### 1. Iniciar o Receptor (Cliente)
Inicie primeiro o cliente para que a porta UDP esteja aberta e aguardando os pacotes RTP:
```bash
./cliente
```
1. Observe a linha em destaque verde: **"Seu IP para o transmissor: <IP_LOCAL>"**.
2. Mantenha a porta UDP padrão `5000` (ou altere se necessário).
3. Selecione a latência desejada para o Jitter Buffer (padrão: `200 ms`).
4. Clique em **"Iniciar Recepção"**.
5. O status passará para *Aguardando áudio na porta UDP 5000...*.

### 2. Iniciar o Transmissor (Servidor)
No computador transmissor (ou em outro terminal na mesma máquina):
```bash
./servidor
```
1. No campo **IP do Receptor**, informe o IP exibido pelo cliente (use `127.0.0.1` se estiver testando no mesmo computador).
2. Verifique se a **Porta UDP** é a mesma (`5000`).
3. O arquivo padrão `audio.mp3` já virá selecionado (é possível escolher outro arquivo de áudio clicando em "Selecionar...").
4. Clique em **"Iniciar Transmissão"**.
5. A barra de progresso acompanhará em tempo real os megabytes transmitidos e o áudio começará a tocar imediatamente nos alto-falantes do cliente.
6. As estatísticas de pacotes e jitter médio serão atualizadas dinamicamente no painel do cliente.

---

## Destaques Técnicos

1. **Loop de Eventos Não-Bloqueante**: Ambas as aplicações utilizam `gst_bus_add_watch()` e `g_timeout_add()` integrados ao `gtk_main()`, garantindo interfaces gráficas fluidas, responsivas e sem congelamento durante o streaming.
2. **Robustez de Áudio no Linux**: A criação do elemento de saída sonoro testa iterativamente drivers de áudio (`alsasink`, `pulsesink`, `autoaudiosink`), contornando problemas de deadlock comuns em servidores PipeWire.
3. **Resiliência a Jitter de Rede**: O elemento `rtpjitterbuffer` reordena pacotes que chegam fora de ordem e absorve variações de tempo de trânsito da rede, registrando métricas precisas para análise de desempenho.
4. **Encerramento Limpo**: Liberação adequada de todos os recursos de pipeline com `GST_STATE_NULL`, desconexão de watches de bus e remoção de timers do GLib ao interromper ou fechar a janela.
