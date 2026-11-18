# Atividade Prática - Streaming entre Dispositivos (18/11/2026)

Aplicação de transmissão e reprodução de áudio ao vivo via RTP/UDP entre computadores na rede local com compensação de jitter e controle de latência. O som é decodificado e reproduzido diretamente no alto-falante em tempo real.

## Dependências

```bash
sudo apt update && sudo apt install -y libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly
```

## Compilação

```bash
gcc streaming_audio.c -o streaming_audio $(pkg-config --cflags --libs gstreamer-1.0) -Wall
```

## Execução entre Dois Computadores

### 1. No Receptor (Dispositivo B - Iniciar primeiro)
Descubra o IPv4 da interface de rede (`ip a` ou `hostname -I`).

- **Via Linha de Comando**:
  ```bash
  # Teste 1 (latência 50 ms):
  ./streaming_audio -r 5000 50

  # Teste 2 (latência 200 ms):
  ./streaming_audio -r 5000 200
  ```
- **Via Modo Interativo**:
  ```bash
  ./streaming_audio
  # Escolha: 2 -> Porta: 5000 -> Latencia: 50 ou 200
  ```
O áudio toca no alto-falante em tempo real enquanto os pacotes chegam. Ao fim, o relatório com as estatísticas de jitter e perdas é gravado em `log_receptor_<latencia>ms.txt`.

---

### 2. No Transmissor (Dispositivo A)
Informe o IP real do Dispositivo B e a mesma porta configurada:

- **Via Linha de Comando**:
  ```bash
  ./streaming_audio -t <IP_DO_RECEPTOR> 5000 audio.mp3
  # Exemplo: ./streaming_audio -t 192.168.10.2 5000 audio.mp3
  ```
- **Via Modo Interativo**:
  ```bash
  ./streaming_audio
  # Escolha: 1 -> IP: <IP_DO_RECEPTOR> -> Porta: 5000
  ```

---

### Dicas para Transmissão entre Dispositivos Diferentes

1. **Teste de Conectividade**: Antes de transmitir, teste no Dispositivo A se o Dispositivo B responde:
   ```bash
   ping <IP_DO_RECEPTOR>
   ```
2. **Firewall no Receptor**: Se os pacotes não chegarem, libere a porta UDP no Dispositivo B:
   ```bash
   sudo ufw allow 5000/udp
   ```
3. **Isolamento de Wi-Fi institucional (AP Isolation)**: Redes universitárias (como eduroam ou Wi-Fi institucional) costumam bloquear comunicação direta entre clientes. Se o ping falhar:
   - Conecte as duas máquinas via **cabo Ethernet direto**, definindo IPs fixos (ex: `192.168.10.1` e `192.168.10.2`, máscara `255.255.255.0`).
   - Ou roteie um **ponto de acesso Wi-Fi (hotspot)** a partir de um celular e conecte ambos os notebooks nele.
