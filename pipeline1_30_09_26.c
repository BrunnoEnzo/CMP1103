#include <gst/gst.h>

#ifdef __APPLE__
#endif

typedef struct _CustomData
{
    GstElement *pipeline;
    GstElement *video_convert;
} CustomData;

static void pad_added_handler(GstElement *src, GstPad *pad, CustomData *data);

int tutorial_main(int argc, char *argv[])
{
    CustomData data;
    GstElement *source, *scale, *rate, *filter, *convert2, *sink;
    GstBus *bus;
    GstMessage *msg;
    GstStateChangeReturn ret;
    GstCaps *caps;

    gst_init(&argc, &argv);

    source = gst_element_factory_make("uridecodebin", "source");
    data.video_convert = gst_element_factory_make("videoconvert", "convert1");
    scale = gst_element_factory_make("videoscale", "scale");
    rate = gst_element_factory_make("videorate", "rate");
    filter = gst_element_factory_make("capsfilter", "filter");
    convert2 = gst_element_factory_make("videoconvert", "convert2");
    sink = gst_element_factory_make("autovideosink", "sink");
    data.pipeline = gst_pipeline_new("test-pipeline");

    if (!data.pipeline || !source || !data.video_convert || !scale || !rate || !filter || !convert2 || !sink)
    {
        g_printerr("Nem todos os elementos puderam ser criados.\n");
        return -1;
    }

    g_object_set(source, "uri", "https://gstreamer.freedesktop.org/data/media/sintel_trailer-480p.webm", NULL);

    caps = gst_caps_from_string("video/x-raw, width=320, height=180, framerate=12/1, format=GRAY8");
    g_object_set(filter, "caps", caps, NULL);
    gst_caps_unref(caps);

    gst_bin_add_many(GST_BIN(data.pipeline), source, data.video_convert, scale, rate, filter, convert2, sink, NULL);
    if (gst_element_link_many(data.video_convert, scale, rate, filter, convert2, sink, NULL) != TRUE)
    {
        g_printerr("Os elementos de processamento não puderam ser linkados.\n");
        gst_object_unref(data.pipeline);
        return -1;
    }

    g_signal_connect(source, "pad-added", G_CALLBACK(pad_added_handler), &data);

    ret = gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("Não foi possível mudar o estado da pipeline para PLAYING.\n");
        gst_object_unref(data.pipeline);
        return -1;
    }

    bus = gst_element_get_bus(data.pipeline);
    msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                                     GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

    if (msg != NULL)
    {
        GError *err;
        gchar *debug_info;

        switch (GST_MESSAGE_TYPE(msg))
        {
        case GST_MESSAGE_ERROR:
            gst_message_parse_error(msg, &err, &debug_info);
            g_printerr("Erro recebido do elemento %s: %s\n",
                       GST_OBJECT_NAME(msg->src), err->message);
            g_printerr("Informação de depuração: %s\n",
                       debug_info ? debug_info : "nenhuma");
            g_clear_error(&err);
            g_free(debug_info);
            break;
        case GST_MESSAGE_EOS:
            g_print("Fim da stream (EOS) alcançado.\n");
            break;
        default:
            g_printerr("Mensagem inesperada recebida.\n");
            break;
        }
        gst_message_unref(msg);
    }

    gst_object_unref(bus);
    gst_element_set_state(data.pipeline, GST_STATE_NULL);
    gst_object_unref(data.pipeline);
    return 0;
}

static void pad_added_handler(GstElement *src, GstPad *new_pad, CustomData *data)
{
    GstPad *sink_pad = gst_element_get_static_pad(data->video_convert, "sink");
    GstPadLinkReturn ret;
    GstCaps *new_pad_caps = NULL;
    GstStructure *new_pad_struct = NULL;
    const gchar *new_pad_type = NULL;

    g_print("Recebido novo pad '%s' do elemento '%s'.\n", GST_PAD_NAME(new_pad), GST_ELEMENT_NAME(src));

    if (gst_pad_is_linked(sink_pad))
    {
        g_print("Pad já está conectado. Ignorando.\n");
        goto exit;
    }

    new_pad_caps = gst_pad_get_current_caps(new_pad);
    new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
    new_pad_type = gst_structure_get_name(new_pad_struct);

    if (g_str_has_prefix(new_pad_type, "video/x-raw"))
    {
        ret = gst_pad_link(new_pad, sink_pad);
        if (GST_PAD_LINK_FAILED(ret))
        {
            g_print("Tipo é '%s' mas o link falhou.\n", new_pad_type);
        }
        else
        {
            g_print("Link bem sucedido (tipo '%s').\n", new_pad_type);
        }
    }
    else
    {
        g_print("Pad contém tipo '%s' que não é o nosso foco (ignorado).\n", new_pad_type);
    }

exit:
    if (new_pad_caps != NULL)
        gst_caps_unref(new_pad_caps);
    gst_object_unref(sink_pad);
}

int main(int argc, char *argv[])
{
#if defined(__APPLE__) && TARGET_OS_MAC && !TARGET_OS_IPHONE
    return gst_macos_main((GstMainFunc)tutorial_main, argc, argv, NULL);
#else
    return tutorial_main(argc, argv);
#endif
}