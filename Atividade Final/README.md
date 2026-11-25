# Atividade Final - Streaming Multimídia com Interface Gráfica (GTK 3)

Evolução do sistema de streaming multimídia em C/GStreamer com interfaces gráficas GTK 3 para o **Servidor (Transmissor)** e **Cliente (Receptor)**, permitindo transmissão e exibição de vídeo H.264 sobre MPEG-TS/UDP em tempo real, tanto em `127.0.0.1` quanto entre computadores físicos na rede local.

---

## Estrutura dos Arquivos

- **`servidor.c`**: Aplicação GTK 3 do transmissor. Permite configurar o IP de destino, porta UDP, resolução, taxa de quadros (FPS), bitrate H.264, preset de CPU e padrão de vídeo, com botões Iniciar/Interromper e status em tempo real.
- **`cliente.c`**: Aplicação GTK 3 do receptor. Exibe automaticamente o IP local da máquina, porta de escuta UDP, botões Iniciar/Interromper, status da conexão e **área de vídeo incorporada diretamente na janela** (via plugin `gtksink`, com fallback para `autovideosink`).
- **`Makefile`**: Script de compilação automatizada com flags otimizadas (`-Wall -Wextra -O2`).
- **`streaming_audio.c`** e **`audio.mp3`**: Código da atividade anterior mantido para referência.
- **`Atividade_Final_Streaming_GStreamer.pdf`**: Guia oficial da atividade prática.

---

## Dependências

No Ubuntu/Debian:
```bash
sudo apt update
sudo apt install -y libgtk-3-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-gtk3
```

No Fedora:
```bash
sudo dnf install -y gtk3-devel gstreamer1-devel gstreamer1-plugins-base-devel gstreamer1-plugins-good gstreamer1-plugins-bad-free gstreamer1-plugins-ugly-free gstreamer1-libav
```

---

## Compilação

Para compilar ambos os programas simultaneamente:
```bash
make
```

Ou compilar manualmente via GCC:
```bash
# Compilar o Servidor (Transmissor):
gcc -Wall -Wextra -O2 servidor.c -o servidor $(pkg-config --cflags --libs gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0)

# Compilar o Cliente (Receptor):
gcc -Wall -Wextra -O2 cliente.c -o cliente $(pkg-config --cflags --libs gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0)
```

---

## Execução

### 1. No Computador Receptor (Dispositivo B - Iniciar Primeiro)

```bash
./cliente
```
1. A janela exibirá no topo o **IP desta máquina** (ex: `192.168.1.150`).
2. Confirme a porta UDP (padrão: `5000`).
3. Clique em **"Iniciar Recepção"**.
4. O status mudará para *Aguardando transmissão na porta UDP 5000...*. Quando o servidor começar a transmitir, o vídeo aparecerá reproduzido diretamente na área de exibição da janela.

---

### 2. No Computador Transmissor (Dispositivo A)

```bash
./servidor
```
1. No campo **IP do Receptor**, informe o IP exibido na janela do Cliente (ou `127.0.0.1` para teste no mesmo computador).
2. Ajuste a **Porta UDP** para a mesma configurada no cliente (`5000`).
3. Selecione os **Parâmetros de Qualidade**:
   - **Resolução**: `640x360` (padrão), `320x240` (menor largura de banda) ou `1280x720` (alta definição).
   - **Taxa de Quadros (FPS)**: `15`, `25` ou `30` fps.
   - **Bitrate H.264**: `200`, `500` ou `1500` kbps.
   - **Velocidade do Encoder (CPU)**: `ultrafast` (menor latência/CPU), `fast` ou `medium`.
   - **Padrão de Vídeo**: Bola Saltitante (`ball`), Barras de Cores (`smpte`) ou Ruído (`snow`).
4. Clique em **"Iniciar Transmissão"**.
5. O status ficará verde indicando *Transmitindo para <IP>:<Porta>*.

---

## Destaques da Implementação

1. **Loop de Eventos Não Bloqueante**: A comunicação com as pipelines do GStreamer utiliza `gst_bus_add_watch()` associado ao loop principal da GLib (`g_main_loop`), garantindo que a interface gráfica GTK nunca trave ou congele.
2. **Reprodução Integrada (`gtksink`)**: O cliente utiliza o sink oficial do GTK para incorporar os frames de vídeo renderizados diretamente em um container `GtkFrame`, eliminando a necessidade de janelas externas separadas.
3. **Pipeline Otimizada**: Conforme investigado na Parte 1, o codificador `x264enc` utiliza `tune=zerolatency` e `key-int-max=FPS`, `h264parse config-interval=-1`, `mpegtsmux alignment=7` e `udpsink sync=false` para garantir fluidez máxima e latência mínima pela rede.
4. **Gerenciamento de Recursos**: Ao clicar em "Interromper" ou fechar a janela, as pipelines são levadas com segurança para o estado `GST_STATE_NULL` e devidamente liberadas da memória.
