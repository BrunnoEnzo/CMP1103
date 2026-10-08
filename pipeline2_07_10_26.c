#include <gst/gst.h>
#include <stdio.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

// Estrutura para os dados da pipeline
typedef struct _CustomData
{
    GstElement *pipeline;
    GstElement *video_queue;
    GstElement *audio_queue;
} CustomData;

static void pad_added_handler(GstElement *src, GstPad *pad, CustomData *data);

int pipeline_main(int argc, char *argv[])
{
    CustomData data;
    GstElement *source;
    GstElement *video_convert, *video_sink;
    GstElement *audio_convert1, *audio_resample1, *audio_filter;
    GstElement *audio_convert2, *audio_resample2, *audio_sink;
    GstBus *bus;
    GstMessage *msg;
    GstStateChangeReturn ret;
    GstCaps *audio_caps;
    char choice[16];

    gst_init(&argc, &argv);

    printf("Opcao 1: 44100 Hz, 16 bits (S16LE), estereo\n");
    printf("Opcao 2: 8000 Hz, 8 bits (U8), mono\n");
    printf("Escolha uma opcao (1 ou 2): ");
    fflush(stdout);

    // Seleção da configuração de áudio PCM
    if (fgets(choice, sizeof(choice), stdin) != NULL && choice[0] == '2')
    {
        audio_caps = gst_caps_from_string("audio/x-raw, format=U8, rate=8000, channels=1");
    }
    else
    {
        audio_caps = gst_caps_from_string("audio/x-raw, format=S16LE, rate=44100, channels=2");
    }

    // Criação dos elementos da pipeline
    source = gst_element_factory_make("uridecodebin", "source");

    data.video_queue = gst_element_factory_make("queue", "video_queue");
    video_convert = gst_element_factory_make("videoconvert", "video_convert");
    video_sink = gst_element_factory_make("autovideosink", "video_sink");

    data.audio_queue = gst_element_factory_make("queue", "audio_queue");
    audio_convert1 = gst_element_factory_make("audioconvert", "audio_convert1");
    audio_resample1 = gst_element_factory_make("audioresample", "audio_resample1");
    audio_filter = gst_element_factory_make("capsfilter", "audio_filter");
    audio_convert2 = gst_element_factory_make("audioconvert", "audio_convert2");
    audio_resample2 = gst_element_factory_make("audioresample", "audio_resample2");
    audio_sink = gst_element_factory_make("autoaudiosink", "audio_sink");

    data.pipeline = gst_pipeline_new("multimedia-pcm-pipeline");

    if (!data.pipeline || !source ||
        !data.video_queue || !video_convert || !video_sink ||
        !data.audio_queue || !audio_convert1 || !audio_resample1 ||
        !audio_filter || !audio_convert2 || !audio_resample2 || !audio_sink)
    {
        g_printerr("Erro: Nem todos os elementos puderam ser criados.\n");
        return -1;
    }

    gchar *uri = gst_filename_to_uri("Cradles.mp4", NULL);
    g_object_set(source, "uri", uri, NULL);
    g_free(uri);

    // Aplicação do filtro de áudio
    g_object_set(audio_filter, "caps", audio_caps, NULL);
    gst_caps_unref(audio_caps);

    gst_bin_add_many(GST_BIN(data.pipeline),
                     source,
                     data.video_queue, video_convert, video_sink,
                     data.audio_queue, audio_convert1, audio_resample1,
                     audio_filter, audio_convert2, audio_resample2, audio_sink,
                     NULL);

    // Conexão dos elementos nos ramos de vídeo e áudio
    if (gst_element_link_many(data.video_queue, video_convert, video_sink, NULL) != TRUE)
    {
        g_printerr("Erro: Nao foi possivel linkar o ramo de video.\n");
        gst_object_unref(data.pipeline);
        return -1;
    }

    if (gst_element_link_many(data.audio_queue, audio_convert1, audio_resample1,
                              audio_filter, audio_convert2, audio_resample2, audio_sink, NULL) != TRUE)
    {
        g_printerr("Erro: Nao foi possivel linkar o ramo de audio.\n");
        gst_object_unref(data.pipeline);
        return -1;
    }

    g_signal_connect(source, "pad-added", G_CALLBACK(pad_added_handler), &data);

    // Início da reprodução
    ret = gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("Erro: Nao foi possivel mudar o estado da pipeline para PLAYING.\n");
        gst_object_unref(data.pipeline);
        return -1;
    }

    bus = gst_element_get_bus(data.pipeline);
    msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                                     GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

    if (msg != NULL)
    {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
        {
            GError *err;
            gchar *debug_info;
            gst_message_parse_error(msg, &err, &debug_info);
            g_printerr("Erro recebido do elemento '%s': %s\n",
                       GST_OBJECT_NAME(msg->src), err->message);
            g_printerr("Informacao de depuracao: %s\n",
                       debug_info ? debug_info : "nenhuma");
            g_clear_error(&err);
            g_free(debug_info);
        }
        gst_message_unref(msg);
    }

    gst_object_unref(bus);
    gst_element_set_state(data.pipeline, GST_STATE_NULL);
    gst_object_unref(data.pipeline);
    return 0;
}

// Conexão dinâmica dos fluxos de vídeo e áudio
static void pad_added_handler(GstElement *src, GstPad *new_pad, CustomData *data)
{
    GstCaps *new_pad_caps = NULL;
    GstStructure *new_pad_struct = NULL;
    const gchar *new_pad_type = NULL;
    GstPad *sink_pad = NULL;

    new_pad_caps = gst_pad_get_current_caps(new_pad);
    if (!new_pad_caps)
    {
        new_pad_caps = gst_pad_query_caps(new_pad, NULL);
    }

    new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
    new_pad_type = gst_structure_get_name(new_pad_struct);

    if (g_str_has_prefix(new_pad_type, "video/x-raw"))
    {
        sink_pad = gst_element_get_static_pad(data->video_queue, "sink");
        if (!gst_pad_is_linked(sink_pad))
        {
            if (GST_PAD_LINK_FAILED(gst_pad_link(new_pad, sink_pad)))
            {
                g_printerr("Erro: Falha ao linkar pad de video.\n");
            }
        }
    }
    else if (g_str_has_prefix(new_pad_type, "audio/x-raw"))
    {
        sink_pad = gst_element_get_static_pad(data->audio_queue, "sink");
        if (!gst_pad_is_linked(sink_pad))
        {
            if (GST_PAD_LINK_FAILED(gst_pad_link(new_pad, sink_pad)))
            {
                g_printerr("Erro: Falha ao linkar pad de audio.\n");
            }
        }
    }

    if (new_pad_caps != NULL)
        gst_caps_unref(new_pad_caps);
    if (sink_pad != NULL)
        gst_object_unref(sink_pad);
}

int main(int argc, char *argv[])
{
#if defined(__APPLE__) && TARGET_OS_MAC && !TARGET_OS_IPHONE
    return gst_macos_main((GstMainFunc)pipeline_main, argc, argv, NULL);
#else
    return pipeline_main(argc, argv);
#endif
}