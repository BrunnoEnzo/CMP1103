# Atividade 1 - 30/09/2026

Pipeline de vídeo com decodificação dinâmica (`uridecodebin`), redimensionamento (`videoscale`), ajuste de taxa (`videorate`) e conversão para escala de cinza (`capsfilter format=GRAY8`).

## Compilação

```bash
gcc pipeline1_30_09_26.c -o pipeline1_30_09_26 $(pkg-config --cflags --libs gstreamer-1.0) -Wall
```

## Execução

```bash
./pipeline1_30_09_26
```
