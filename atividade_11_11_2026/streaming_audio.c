#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

// Definição dos caminhos dos arquivos de áudio e log no código
#define ARQUIVO_ORIGEM "audio.mp3"
#define ARQUIVO_DESTINO "audio_recebido.mp3"
#define ARQUIVO_LOG "log_receptor.txt"

// Flag de controle para encerramento gracioso via sinal
static volatile gboolean g_rodando = TRUE;

static void sigint_handler(int sig)
{
    g_rodando = FALSE;
}

// Estrutura para os dados da pipeline do transmissor
typedef struct _TransmitterData
{
    GstElement *pipeline;
    GstElement *audio_convert;
} TransmitterData;

// Conexão dinâmica do pad de áudio decodificado
static void pad_added_handler(GstElement *src, GstPad *new_pad, TransmitterData *data)
{
    GstPad *sink_pad = gst_element_get_static_pad(data->audio_convert, "sink");
    GstPadLinkReturn ret;
    GstCaps *new_pad_caps = NULL;
    GstStructure *new_pad_struct = NULL;
    const gchar *new_pad_type = NULL;

    if (gst_pad_is_linked(sink_pad))
    {
        goto exit;
    }

    new_pad_caps = gst_pad_get_current_caps(new_pad);
    if (!new_pad_caps)
    {
        new_pad_caps = gst_pad_query_caps(new_pad, NULL);
    }

    new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
    new_pad_type = gst_structure_get_name(new_pad_struct);

    if (g_str_has_prefix(new_pad_type, "audio/x-raw"))
    {
        ret = gst_pad_link(new_pad, sink_pad);
        if (GST_PAD_LINK_FAILED(ret))
        {
            g_printerr("Erro ao conectar pad de audio.\n");
        }
    }

exit:
    if (new_pad_caps != NULL)
        gst_caps_unref(new_pad_caps);
    if (sink_pad != NULL)
        gst_object_unref(sink_pad);
}

// Criação do elemento de saída de áudio compatível com o sistema
static GstElement *criar_audio_sink(void)
{
    GstElement *sink = NULL;
    const char *sinks[] = {"alsasink", "pulsesink", "autoaudiosink", NULL};

    for (int i = 0; sinks[i] != NULL; i++)
    {
        sink = gst_element_factory_make(sinks[i], "sink");
        if (sink)
        {
            if (gst_element_set_state(sink, GST_STATE_READY) == GST_STATE_CHANGE_SUCCESS)
            {
                gst_element_set_state(sink, GST_STATE_NULL);
                return sink;
            }
            gst_object_unref(sink);
            sink = NULL;
        }
    }
    return gst_element_factory_make("autoaudiosink", "sink");
}

// Execução da função do transmissor RTP
int executar_transmissor(void)
{
    TransmitterData data;
    GstElement *source, *resample, *filter, *payloader, *sink;
    GstBus *bus;
    GstMessage *msg;
    GstStateChangeReturn ret;
    GstCaps *caps;
    char buffer[128];
    char ip[64] = "127.0.0.1";
    int port = 5000;

    FILE *f = fopen(ARQUIVO_ORIGEM, "rb");
    if (!f)
    {
        g_printerr("Erro: Arquivo %s nao encontrado no diretorio atual.\n", ARQUIVO_ORIGEM);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fclose(f);
    if (fsize <= 0)
    {
        g_printerr("Erro: O arquivo %s esta vazio (0 bytes).\n", ARQUIVO_ORIGEM);
        return -1;
    }

    printf("IP do receptor [127.0.0.1]: ");
    fflush(stdout);
    if (fgets(buffer, sizeof(buffer), stdin) && buffer[0] != '\n')
    {
        buffer[strcspn(buffer, "\r\n")] = 0;
        if (strlen(buffer) > 0)
            strncpy(ip, buffer, sizeof(ip) - 1);
    }

    printf("Porta: ");
    fflush(stdout);
    if (fgets(buffer, sizeof(buffer), stdin) && buffer[0] != '\n')
    {
        int p = atoi(buffer);
        if (p > 0)
            port = p;
    }

    // Criação dos elementos da pipeline do transmissor
    source = gst_element_factory_make("uridecodebin", "source");
    data.audio_convert = gst_element_factory_make("audioconvert", "convert");
    resample = gst_element_factory_make("audioresample", "resample");
    filter = gst_element_factory_make("capsfilter", "filter");
    payloader = gst_element_factory_make("rtpL16pay", "payloader");
    sink = gst_element_factory_make("udpsink", "sink");
    data.pipeline = gst_pipeline_new("pipeline-transmissor");

    if (!data.pipeline || !source || !data.audio_convert || !resample || !filter || !payloader || !sink)
    {
        g_printerr("Erro: Falha ao criar elementos do transmissor.\n");
        return -1;
    }

    // Abertura do arquivo de áudio local
    gchar *uri = gst_filename_to_uri(ARQUIVO_ORIGEM, NULL);
    g_object_set(source, "uri", uri, NULL);
    g_free(uri);

    // Configuração dos caps e propriedades de rede
    caps = gst_caps_from_string("audio/x-raw, format=S16BE, channels=2, rate=44100");
    g_object_set(filter, "caps", caps, NULL);
    gst_caps_unref(caps);

    g_object_set(sink, "host", ip, "port", port, "sync", TRUE, NULL);

    // Montagem e conexão da pipeline
    gst_bin_add_many(GST_BIN(data.pipeline), source, data.audio_convert, resample, filter, payloader, sink, NULL);
    if (gst_element_link_many(data.audio_convert, resample, filter, payloader, sink, NULL) != TRUE)
    {
        g_printerr("Erro ao linkar elementos do transmissor.\n");
        gst_object_unref(data.pipeline);
        return -1;
    }

    g_signal_connect(source, "pad-added", G_CALLBACK(pad_added_handler), &data);

    // Início da transmissão
    printf("Transmitindo %s para %s:%d...\n", ARQUIVO_ORIGEM, ip, port);
    fflush(stdout);

    ret = gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("Erro ao iniciar reproducao no transmissor.\n");
        gst_object_unref(data.pipeline);
        return -1;
    }

    // Monitoramento do barramento e progresso da transmissao
    bus = gst_element_get_bus(data.pipeline);
    gboolean rodando_tx = TRUE;

    while (rodando_tx)
    {
        msg = gst_bus_timed_pop_filtered(bus, 500 * GST_MSECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
        if (msg != NULL)
        {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
            {
                GError *err;
                gchar *debug_info;
                gst_message_parse_error(msg, &err, &debug_info);
                g_printerr("\nErro no transmissor: %s\n", err->message);
                g_clear_error(&err);
                g_free(debug_info);
                rodando_tx = FALSE;
            }
            else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
            {
                printf("\nTransmissao concluida.\n");
                rodando_tx = FALSE;
            }
            gst_message_unref(msg);
        }
        else
        {
            gint64 pos = 0, dur = 0;
            if (gst_element_query_position(data.pipeline, GST_FORMAT_TIME, &pos) &&
                gst_element_query_duration(data.pipeline, GST_FORMAT_TIME, &dur) && dur > 0)
            {
                printf("\rTransmitindo: %02ld:%02ld / %02ld:%02ld",
                       (long)((pos / GST_SECOND) / 60), (long)((pos / GST_SECOND) % 60),
                       (long)((dur / GST_SECOND) / 60), (long)((dur / GST_SECOND) % 60));
                fflush(stdout);
            }
        }
    }

    // Liberação de recursos
    gst_object_unref(bus);
    gst_element_set_state(data.pipeline, GST_STATE_NULL);
    gst_object_unref(data.pipeline);
    return 0;
}

// Execução da função do receptor RTP
int executar_receptor(void)
{
    GstElement *pipeline, *src, *jitterbuffer, *depayloader, *convert, *resample, *encoder, *sink;
    GstBus *bus;
    GstMessage *msg;
    GstStateChangeReturn ret;
    GstCaps *caps;
    char buffer[128];
    int port = 5000;
    int latency = 200;

    printf("Porta: ");
    fflush(stdout);
    if (fgets(buffer, sizeof(buffer), stdin) && buffer[0] != '\n')
    {
        int p = atoi(buffer);
        if (p > 0)
            port = p;
    }

    printf("Latencia (ms): ");
    fflush(stdout);
    if (fgets(buffer, sizeof(buffer), stdin) && buffer[0] != '\n')
    {
        int l = atoi(buffer);
        if (l > 0)
            latency = l;
    }

    // Criação dos elementos da pipeline de recepção e gravação do áudio
    src = gst_element_factory_make("udpsrc", "udpsrc");
    jitterbuffer = gst_element_factory_make("rtpjitterbuffer", "jitterbuffer");
    depayloader = gst_element_factory_make("rtpL16depay", "depayloader");
    convert = gst_element_factory_make("audioconvert", "convert");
    resample = gst_element_factory_make("audioresample", "resample");
    encoder = gst_element_factory_make("lamemp3enc", "encoder");
    sink = gst_element_factory_make("filesink", "sink");
    pipeline = gst_pipeline_new("pipeline-receptor");

    if (!pipeline || !src || !jitterbuffer || !depayloader || !convert || !resample || !encoder || !sink)
    {
        g_printerr("Erro: Falha ao criar elementos do receptor.\n");
        return -1;
    }

    // Configuração dos caps, jitterbuffer e arquivo de saída
    caps = gst_caps_from_string("application/x-rtp, media=audio, clock-rate=44100, encoding-name=L16, channels=(int)2, payload=96");
    g_object_set(src, "port", port, "address", "0.0.0.0", "caps", caps, "timeout", (guint64)3000000000ULL, NULL);
    gst_caps_unref(caps);

    g_object_set(jitterbuffer, "latency", latency, "do-lost", TRUE, NULL);
    g_object_set(encoder, "target", 1, "bitrate", 192, "cbr", TRUE, NULL);
    g_object_set(sink, "location", ARQUIVO_DESTINO, NULL);

    // Montagem e conexão da pipeline
    gst_bin_add_many(GST_BIN(pipeline), src, jitterbuffer, depayloader, convert, resample, encoder, sink, NULL);
    if (gst_element_link_many(src, jitterbuffer, depayloader, convert, resample, encoder, sink, NULL) != TRUE)
    {
        g_printerr("Erro ao linkar elementos do receptor.\n");
        gst_object_unref(pipeline);
        return -1;
    }

    // Configura tratamento de sinal para encerramento gracioso via Ctrl+C
    g_rodando = TRUE;
    signal(SIGINT, sigint_handler);

    // Início da recepção e gravação
    printf("Aguardando transmissao na porta %d...\n", port);
    fflush(stdout);

    ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("Erro ao iniciar pipeline do receptor.\n");
        gst_object_unref(pipeline);
        return -1;
    }

    // Loop de monitoramento de pacotes e estatísticas durante o envio
    bus = gst_element_get_bus(pipeline);
    gboolean recebendo = FALSE;
    guint64 pushed = 0, lost = 0, late = 0, duplicates = 0, jitter = 0;

    while (g_rodando)
    {
        msg = gst_bus_timed_pop_filtered(bus, 500 * GST_MSECOND,
                                         GST_MESSAGE_ELEMENT | GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

        GstStructure *stats = NULL;
        g_object_get(jitterbuffer, "stats", &stats, NULL);
        if (stats)
        {
            gst_structure_get_uint64(stats, "num-pushed", &pushed);
            gst_structure_get_uint64(stats, "num-lost", &lost);
            gst_structure_get_uint64(stats, "num-late", &late);
            gst_structure_get_uint64(stats, "num-duplicates", &duplicates);
            gst_structure_get_uint64(stats, "avg-jitter", &jitter);
            gst_structure_free(stats);
        }

        if (pushed > 0)
        {
            if (!recebendo)
            {
                recebendo = TRUE;
                printf("Recebendo dados e salvando em %s...\n", ARQUIVO_DESTINO);
            }
            printf("\rPacotes recebidos: %lu | Perdidos: %lu | Atrasados: %lu | Jitter: %lu ns",
                   (unsigned long)pushed, (unsigned long)lost, (unsigned long)late, (unsigned long)jitter);
            fflush(stdout);
        }

        if (msg != NULL)
        {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
            {
                GError *err;
                gchar *debug_info;
                gst_message_parse_error(msg, &err, &debug_info);
                g_printerr("\nErro no receptor: %s\n", err->message);
                g_clear_error(&err);
                g_free(debug_info);
                g_rodando = FALSE;
            }
            else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
            {
                g_rodando = FALSE;
            }
            else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ELEMENT)
            {
                const GstStructure *s = gst_message_get_structure(msg);
                if (gst_structure_has_name(s, "GstUDPSrcTimeout"))
                {
                    if (recebendo)
                    {
                        g_rodando = FALSE;
                    }
                }
            }
            gst_message_unref(msg);
        }
    }

    printf("\nEnvio concluido. Arquivo salvo com sucesso.\n");

    // Descarrega o encoder com EOS para garantir a gravacao completa do arquivo MP3
    gst_element_send_event(pipeline, gst_event_new_eos());
    GstMessage *eos_msg = gst_bus_timed_pop_filtered(bus, 1 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (eos_msg)
        gst_message_unref(eos_msg);

    // Liberação da pipeline de recepção
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    // Gravação do arquivo de log do receptor
    FILE *log = fopen(ARQUIVO_LOG, "w");
    if (log)
    {
        time_t agora = time(NULL);
        fprintf(log, "=== LOG DO RECEPTOR RTP/UDP ===\n");
        fprintf(log, "Data/Hora: %s", ctime(&agora));
        fprintf(log, "Porta: %d\n", port);
        fprintf(log, "Latencia: %d ms\n", latency);
        fprintf(log, "Arquivo de audio salvo: %s\n", ARQUIVO_DESTINO);
        fprintf(log, "Pacotes recebidos (pushed): %lu\n", (unsigned long)pushed);
        fprintf(log, "Pacotes perdidos (lost): %lu\n", (unsigned long)lost);
        fprintf(log, "Pacotes atrasados (late): %lu\n", (unsigned long)late);
        fprintf(log, "Pacotes duplicados: %lu\n", (unsigned long)duplicates);
        fprintf(log, "Jitter medio: %lu ns\n", (unsigned long)jitter);
        fprintf(log, "Status: Concluido com sucesso\n");
        fclose(log);
    }

    // Reprodução do áudio gravado ao final da transmissão
    if (pushed > 0 && access(ARQUIVO_DESTINO, F_OK) == 0)
    {
        printf("Reproduzindo audio salvo...\n");
        fflush(stdout);

        GstElement *player = gst_element_factory_make("playbin", "player");
        GstElement *audio_sink = criar_audio_sink();
        gchar *uri = gst_filename_to_uri(ARQUIVO_DESTINO, NULL);

        if (player && uri)
        {
            g_object_set(player, "uri", uri, "audio-sink", audio_sink, NULL);
            g_free(uri);

            gst_element_set_state(player, GST_STATE_PLAYING);
            GstBus *play_bus = gst_element_get_bus(player);
            GstMessage *play_msg = gst_bus_timed_pop_filtered(play_bus, GST_CLOCK_TIME_NONE,
                                                             GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

            if (play_msg != NULL)
            {
                gst_message_unref(play_msg);
            }

            gst_object_unref(play_bus);
            gst_element_set_state(player, GST_STATE_NULL);
            gst_object_unref(player);
        }
    }

    printf("Reproducao concluida.\n");
    return 0;
}

// Ponto de entrada principal
int main(int argc, char *argv[])
{
    char opcao[16];

    gst_init(&argc, &argv);

    printf("1 - Transmissor\n");
    printf("2 - Receptor\n");
    printf("Escolha: ");
    fflush(stdout);

    if (fgets(opcao, sizeof(opcao), stdin) != NULL)
    {
        if (opcao[0] == '1')
        {
            return executar_transmissor();
        }
        else if (opcao[0] == '2')
        {
            return executar_receptor();
        }
    }

    printf("Opcao invalida.\n");
    return 0;
}
