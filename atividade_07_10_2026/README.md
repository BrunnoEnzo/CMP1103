# Atividade 2 - 07/10/2026

Pipeline multimídia com ramificação de fluxos de áudio e vídeo a partir do arquivo local `Cradles.mp4`, permitindo alternar configurações de amostragem PCM.

## Compilação

```bash
gcc pipeline2_07_10_26.c -o pipeline2_07_10_26 $(pkg-config --cflags --libs gstreamer-1.0) -Wall
```

## Execução

```bash
./pipeline2_07_10_26
```

Selecione a configuração desejada no menu:
- `1`: 44100 Hz, 16 bits (S16LE), estéreo
- `2`: 8000 Hz, 8 bits (U8), mono
