#include <gtk/gtk.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>

// Estrutura com os componentes e estado da aplicação Cliente (Receptor)
typedef struct {
    GtkWidget *window;
    GtkWidget *spin_porta;
    GtkWidget *btn_iniciar;
    GtkWidget *btn_interromper;
    GtkWidget *lbl_status;
    GtkWidget *lbl_ip_local;
    GtkWidget *video_frame;
    GtkWidget *video_box;
    GtkWidget *video_widget;
    GtkWidget *lbl_placeholder;

    GstElement *pipeline;
    guint bus_watch_id;
    gboolean recebendo;
} ClienteApp;

// Detecta e retorna os endereços IPv4 das interfaces ativas do dispositivo
static gchar *obter_ips_locais(void) {
    GString *str = g_string_new("");
    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) == 0) {
        for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
                if (ifa->ifa_flags & IFF_LOOPBACK)
                    continue;
                if (strncmp(ifa->ifa_name, "docker", 6) == 0 || strncmp(ifa->ifa_name, "br-", 3) == 0)
                    continue;
                struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(sa->sin_addr), ip, INET_ADDRSTRLEN);
                if (str->len > 0) g_string_append(str, " | ");
                g_string_append_printf(str, "%s (%s)", ip, ifa->ifa_name);
            }
        }
        freeifaddrs(ifap);
    }
    if (str->len == 0) g_string_assign(str, "127.0.0.1 (local)");
    return g_string_free(str, FALSE);
}

// Interrompe a recepção, remove o widget de vídeo e restaura os botões
static void interromper_recepcao(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ClienteApp *app = (ClienteApp *)user_data;
    if (!app->recebendo && !app->pipeline) return;

    if (app->bus_watch_id > 0) {
        g_source_remove(app->bus_watch_id);
        app->bus_watch_id = 0;
    }

    if (app->pipeline) {
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
    }

    if (app->video_widget) {
        gtk_container_remove(GTK_CONTAINER(app->video_box), app->video_widget);
        app->video_widget = NULL;
    }
    gtk_widget_show(app->lbl_placeholder);

    app->recebendo = FALSE;
    gtk_widget_set_sensitive(app->btn_iniciar, TRUE);
    gtk_widget_set_sensitive(app->btn_interromper, FALSE);
    gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#555555'>Status: <b>Parado</b></span>");
}

// Callback assíncrono do GstBus para tratamento de mensagens no loop principal da GLib
static gboolean bus_callback_cliente(GstBus *bus, GstMessage *msg, gpointer user_data) {
    (void)bus;
    ClienteApp *app = (ClienteApp *)user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("Erro no receptor de vídeo: %s\n", err->message);
            gchar *msg_err = g_strdup_printf("<span foreground='#c62828'><b>Erro na recepção: %s</b></span>", err->message);
            gtk_label_set_markup(GTK_LABEL(app->lbl_status), msg_err);
            g_free(msg_err);
            g_error_free(err);
            g_free(debug);
            interromper_recepcao(NULL, app);
            break;
        }
        case GST_MESSAGE_EOS:
            gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#555555'>Status: <b>Fim da transmissão remota (EOS)</b></span>");
            interromper_recepcao(NULL, app);
            break;
        case GST_MESSAGE_ELEMENT: {
            // Monitora mensagens de decodificação e buffers recebidos
            const GstStructure *s = gst_message_get_structure(msg);
            if (s && g_str_has_prefix(gst_structure_get_name(s), "GstUDPSrc")) {
                // Mensagem do socket UDP
            }
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(app->pipeline)) {
                GstState old_s, new_s, pending_s;
                gst_message_parse_state_changed(msg, &old_s, &new_s, &pending_s);
                if (new_s == GST_STATE_PLAYING) {
                    gtk_label_set_markup(GTK_LABEL(app->lbl_status),
                        "<span foreground='#2e7d32'><b>Status: Reproduzindo vídeo em tempo real</b></span>");
                }
            }
            break;
        }
        default:
            break;
    }
    return TRUE;
}

// Inicia a recepção UDP e conecta a saída ao elemento de vídeo
static void iniciar_recepcao(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ClienteApp *app = (ClienteApp *)user_data;
    if (app->recebendo) return;

    int porta = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_porta));

    // Desafio adicional: Incorpar a reprodução de vídeo à janela do cliente via gtksink
    gboolean tem_gtksink = (gst_element_factory_find("gtksink") != NULL);

    gchar *pipeline_desc;
    if (tem_gtksink) {
        pipeline_desc = g_strdup_printf(
            "udpsrc port=%d caps=\"video/mpegts\" ! "
            "tsdemux name=demux ! "
            "h264parse ! "
            "avdec_h264 ! "
            "videoconvert ! "
            "gtksink name=vsink sync=false",
            porta
        );
    } else {
        pipeline_desc = g_strdup_printf(
            "udpsrc port=%d caps=\"video/mpegts\" ! "
            "tsdemux name=demux ! "
            "h264parse ! "
            "avdec_h264 ! "
            "videoconvert ! "
            "autovideosink sync=false",
            porta
        );
    }

    GError *error = NULL;
    app->pipeline = gst_parse_launch(pipeline_desc, &error);
    g_free(pipeline_desc);

    if (!app->pipeline || error) {
        gchar *msg_err = g_strdup_printf("<span foreground='#c62828'><b>Erro ao criar pipeline: %s</b></span>",
                                         error ? error->message : "Desconhecido");
        gtk_label_set_markup(GTK_LABEL(app->lbl_status), msg_err);
        g_free(msg_err);
        if (error) g_error_free(error);
        return;
    }

    // Se gtksink estiver em uso, extrai o widget de vídeo e embute na janela
    if (tem_gtksink) {
        GstElement *vsink = gst_bin_get_by_name(GST_BIN(app->pipeline), "vsink");
        if (vsink) {
            g_object_get(vsink, "widget", &app->video_widget, NULL);
            gst_object_unref(vsink);

            if (app->video_widget) {
                gtk_widget_hide(app->lbl_placeholder);
                gtk_box_pack_start(GTK_BOX(app->video_box), app->video_widget, TRUE, TRUE, 0);
                gtk_widget_show_all(app->video_box);
            }
        }
    }

    GstBus *bus = gst_element_get_bus(app->pipeline);
    app->bus_watch_id = gst_bus_add_watch(bus, bus_callback_cliente, app);
    gst_object_unref(bus);

    GstStateChangeReturn ret = gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#c62828'><b>Falha ao vincular porta UDP (em uso?)</b></span>");
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
        return;
    }

    app->recebendo = TRUE;
    gtk_widget_set_sensitive(app->btn_iniciar, FALSE);
    gtk_widget_set_sensitive(app->btn_interromper, TRUE);

    gchar *status_txt = g_strdup_printf(
        "<span foreground='#1565c0'><b>Aguardando transmissão na porta UDP %d...</b></span>", porta
    );
    gtk_label_set_markup(GTK_LABEL(app->lbl_status), status_txt);
    g_free(status_txt);
}

// Encerramento limpo ao fechar a janela
static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    ClienteApp *app = (ClienteApp *)user_data;
    interromper_recepcao(NULL, app);
    gtk_main_quit();
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    gtk_init(&argc, &argv);

    ClienteApp app;
    memset(&app, 0, sizeof(ClienteApp));

    // Janela principal
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Cliente Multimídia — Receptor");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 680, 520);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(app.window), 16);
    g_signal_connect(app.window, "destroy", G_CALLBACK(on_window_destroy), &app);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_add(GTK_CONTAINER(app.window), main_box);

    // Cabeçalho de título
    GtkWidget *lbl_header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_header),
        "<span size='x-large' weight='bold' foreground='#17365D'>Cliente de Recepção de Vídeo</span>\n"
        "<span size='small' foreground='#555555'>Receptor GStreamer com Reprodução Integrada (gtksink)</span>");
    gtk_label_set_xalign(GTK_LABEL(lbl_header), 0.0);
    gtk_box_pack_start(GTK_BOX(main_box), lbl_header, FALSE, FALSE, 0);

    // IP local informativo do receptor
    gchar *ip_local = obter_ips_locais();
    gchar *lbl_ip_text = g_strdup_printf("<span size='small' foreground='#3973AC'>IP deste receptor (informe ao transmissor): <b>%s</b></span>", ip_local);
    app.lbl_ip_local = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app.lbl_ip_local), lbl_ip_text);
    gtk_label_set_xalign(GTK_LABEL(app.lbl_ip_local), 0.0);
    gtk_box_pack_start(GTK_BOX(main_box), app.lbl_ip_local, FALSE, FALSE, 0);
    g_free(ip_local);
    g_free(lbl_ip_text);

    // Separador
    gtk_box_pack_start(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    // Linha de controle da porta UDP e botões
    GtkWidget *ctrl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(main_box), ctrl_box, FALSE, FALSE, 0);

    GtkWidget *lbl_porta = gtk_label_new("Porta UDP de Escuta:");
    app.spin_porta = gtk_spin_button_new_with_range(1024, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.spin_porta), 5000);

    app.btn_iniciar = gtk_button_new_with_label("Iniciar Recepção");
    app.btn_interromper = gtk_button_new_with_label("Interromper");
    gtk_widget_set_sensitive(app.btn_interromper, FALSE);

    gtk_box_pack_start(GTK_BOX(ctrl_box), lbl_porta, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), app.spin_porta, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), app.btn_iniciar, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), app.btn_interromper, TRUE, TRUE, 0);

    g_signal_connect(app.btn_iniciar, "clicked", G_CALLBACK(iniciar_recepcao), &app);
    g_signal_connect(app.btn_interromper, "clicked", G_CALLBACK(interromper_recepcao), &app);

    // Indicador de Estado
    app.lbl_status = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app.lbl_status), "<span foreground='#555555'>Status: <b>Parado</b></span>");
    gtk_box_pack_start(GTK_BOX(main_box), app.lbl_status, FALSE, FALSE, 0);

    // Área incorporada para exibição de vídeo (GtkFrame + GtkBox)
    app.video_frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(app.video_frame), GTK_SHADOW_IN);
    gtk_box_pack_start(GTK_BOX(main_box), app.video_frame, TRUE, TRUE, 0);

    app.video_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app.video_frame), app.video_box);

    app.lbl_placeholder = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app.lbl_placeholder),
        "<span size='medium' foreground='#888888'><i>Aguardando início da recepção...\nO vídeo transmitido aparecerá incorporado nesta área.</i></span>");
    gtk_label_set_justify(GTK_LABEL(app.lbl_placeholder), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(app.video_box), app.lbl_placeholder, TRUE, TRUE, 0);

    gtk_widget_show_all(app.window);
    gtk_main();

    return 0;
}
