#include <gtk/gtk.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>

// Estrutura com os componentes e estado da aplicação Servidor (Transmissor)
typedef struct {
    GtkWidget *window;
    GtkWidget *entry_ip;
    GtkWidget *spin_porta;
    GtkWidget *combo_resolucao;
    GtkWidget *combo_fps;
    GtkWidget *combo_bitrate;
    GtkWidget *combo_preset;
    GtkWidget *combo_padrao;
    GtkWidget *btn_iniciar;
    GtkWidget *btn_interromper;
    GtkWidget *lbl_status;
    GtkWidget *lbl_ip_local;

    GstElement *pipeline;
    guint bus_watch_id;
    gboolean transmitindo;
} ServidorApp;

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

// Interrompe a pipeline e restaura o estado dos botões
static void interromper_transmissao(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ServidorApp *app = (ServidorApp *)user_data;
    if (!app->transmitindo && !app->pipeline) return;

    if (app->bus_watch_id > 0) {
        g_source_remove(app->bus_watch_id);
        app->bus_watch_id = 0;
    }

    if (app->pipeline) {
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
    }

    app->transmitindo = FALSE;
    gtk_widget_set_sensitive(app->btn_iniciar, TRUE);
    gtk_widget_set_sensitive(app->btn_interromper, FALSE);
    gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#555555'>Status: <b>Parado</b></span>");
}

// Callback assíncrono do GstBus para tratamento de mensagens no loop principal da GLib
static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer user_data) {
    (void)bus;
    ServidorApp *app = (ServidorApp *)user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("Erro na pipeline do transmissor: %s\n", err->message);
            gchar *msg_err = g_strdup_printf("<span foreground='#c62828'><b>Erro: %s</b></span>", err->message);
            gtk_label_set_markup(GTK_LABEL(app->lbl_status), msg_err);
            g_free(msg_err);
            g_error_free(err);
            g_free(debug);
            interromper_transmissao(NULL, app);
            break;
        }
        case GST_MESSAGE_EOS:
            gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#555555'>Status: <b>Fim do fluxo (EOS)</b></span>");
            interromper_transmissao(NULL, app);
            break;
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(app->pipeline)) {
                GstState old_s, new_s, pending_s;
                gst_message_parse_state_changed(msg, &old_s, &new_s, &pending_s);
                if (new_s == GST_STATE_PLAYING) {
                    // Estado confirmado pelo GStreamer
                }
            }
            break;
        }
        default:
            break;
    }
    return TRUE;
}

// Inicia a pipeline de transmissão com os parâmetros configurados na interface
static void iniciar_transmissao(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ServidorApp *app = (ServidorApp *)user_data;
    if (app->transmitindo) return;

    const gchar *ip = gtk_entry_get_text(GTK_ENTRY(app->entry_ip));
    if (!ip || strlen(ip) == 0) ip = "127.0.0.1";
    int porta = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_porta));

    // Recupera parâmetros de resolução
    const gchar *res_str = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app->combo_resolucao));
    int largura = 640, altura = 360;
    if (res_str) {
        if (g_str_has_prefix(res_str, "320")) { largura = 320; altura = 240; }
        else if (g_str_has_prefix(res_str, "1280")) { largura = 1280; altura = 720; }
    }

    // Recupera taxa de quadros (framerate)
    const gchar *fps_str = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app->combo_fps));
    int fps = (fps_str) ? atoi(fps_str) : 25;
    if (fps <= 0) fps = 25;

    // Recupera bitrate
    const gchar *bitrate_str = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app->combo_bitrate));
    int bitrate = (bitrate_str) ? atoi(bitrate_str) : 500;
    if (bitrate <= 0) bitrate = 500;

    // Recupera speed-preset do x264enc
    const gchar *preset = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app->combo_preset));
    if (!preset) preset = "ultrafast";

    // Recupera padrão de teste
    const gchar *padrao_sel = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app->combo_padrao));
    const gchar *padrao = "ball";
    if (padrao_sel) {
        if (g_str_has_prefix(padrao_sel, "Barras")) padrao = "smpte";
        else if (g_str_has_prefix(padrao_sel, "Ruído") || g_str_has_prefix(padrao_sel, "Ruido")) padrao = "snow";
    }

    // Pipeline otimizada conforme Parte 1 do roteiro (H.264 + MPEG-TS sobre UDP)
    gchar *pipeline_desc = g_strdup_printf(
        "videotestsrc is-live=true pattern=%s ! "
        "video/x-raw,width=%d,height=%d,framerate=%d/1 ! "
        "videoconvert ! "
        "x264enc tune=zerolatency speed-preset=%s bitrate=%d key-int-max=%d ! "
        "h264parse config-interval=-1 ! "
        "mpegtsmux alignment=7 ! "
        "udpsink host=%s port=%d sync=false",
        padrao, largura, altura, fps, preset, bitrate, fps, ip, porta
    );

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

    GstBus *bus = gst_element_get_bus(app->pipeline);
    app->bus_watch_id = gst_bus_add_watch(bus, bus_callback, app);
    gst_object_unref(bus);

    GstStateChangeReturn ret = gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#c62828'><b>Falha ao iniciar transmissão (PLAYING)</b></span>");
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
        return;
    }

    app->transmitindo = TRUE;
    gtk_widget_set_sensitive(app->btn_iniciar, FALSE);
    gtk_widget_set_sensitive(app->btn_interromper, TRUE);

    gchar *status_txt = g_strdup_printf(
        "<span foreground='#2e7d32'><b>Transmitindo para %s:%d</b> [%dx%d @ %dfps, %dkbps, %s]</span>",
        ip, porta, largura, altura, fps, bitrate, preset
    );
    gtk_label_set_markup(GTK_LABEL(app->lbl_status), status_txt);
    g_free(status_txt);
}

// Encerramento limpo ao fechar a janela
static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    ServidorApp *app = (ServidorApp *)user_data;
    interromper_transmissao(NULL, app);
    gtk_main_quit();
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    gtk_init(&argc, &argv);

    ServidorApp app;
    memset(&app, 0, sizeof(ServidorApp));

    // Janela principal
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Servidor Multimídia — Transmissor");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 520, 480);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(app.window), 16);
    g_signal_connect(app.window, "destroy", G_CALLBACK(on_window_destroy), &app);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_add(GTK_CONTAINER(app.window), main_box);

    // Cabeçalho de título
    GtkWidget *lbl_header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_header),
        "<span size='x-large' weight='bold' foreground='#17365D'>Servidor de Transmissão de Vídeo</span>\n"
        "<span size='small' foreground='#555555'>GStreamer MPEG-TS/UDP com Codificação H.264 em Tempo Real</span>");
    gtk_label_set_xalign(GTK_LABEL(lbl_header), 0.0);
    gtk_box_pack_start(GTK_BOX(main_box), lbl_header, FALSE, FALSE, 0);

    // IP local informativo
    gchar *ip_local = obter_ips_locais();
    gchar *lbl_ip_text = g_strdup_printf("<span size='small' foreground='#3973AC'>IP desta máquina: <b>%s</b></span>", ip_local);
    app.lbl_ip_local = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app.lbl_ip_local), lbl_ip_text);
    gtk_label_set_xalign(GTK_LABEL(app.lbl_ip_local), 0.0);
    gtk_box_pack_start(GTK_BOX(main_box), app.lbl_ip_local, FALSE, FALSE, 0);
    g_free(ip_local);
    g_free(lbl_ip_text);

    // Separador
    gtk_box_pack_start(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    // Grid para configurações de rede e qualidade
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_pack_start(GTK_BOX(main_box), grid, FALSE, FALSE, 0);

    int row = 0;

    // 1. IP de destino
    GtkWidget *lbl_dest_ip = gtk_label_new("IP do Receptor:");
    gtk_label_set_xalign(GTK_LABEL(lbl_dest_ip), 0.0);
    app.entry_ip = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(app.entry_ip), "127.0.0.1");
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.entry_ip), "ex: 192.168.1.50 ou 127.0.0.1");
    gtk_grid_attach(GTK_GRID(grid), lbl_dest_ip, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.entry_ip, 1, row, 1, 1);
    row++;

    // 2. Porta UDP
    GtkWidget *lbl_dest_porta = gtk_label_new("Porta UDP:");
    gtk_label_set_xalign(GTK_LABEL(lbl_dest_porta), 0.0);
    app.spin_porta = gtk_spin_button_new_with_range(1024, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.spin_porta), 5000);
    gtk_grid_attach(GTK_GRID(grid), lbl_dest_porta, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.spin_porta, 1, row, 1, 1);
    row++;

    // 3. Resolução de Vídeo
    GtkWidget *lbl_res = gtk_label_new("Resolução:");
    gtk_label_set_xalign(GTK_LABEL(lbl_res), 0.0);
    app.combo_resolucao = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_resolucao), "640x360 (Padrão)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_resolucao), "320x240 (Baixa)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_resolucao), "1280x720 (HD 720p)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_resolucao), 0);
    gtk_grid_attach(GTK_GRID(grid), lbl_res, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.combo_resolucao, 1, row, 1, 1);
    row++;

    // 4. Taxa de Quadros (Framerate)
    GtkWidget *lbl_fps = gtk_label_new("Taxa de Quadros (FPS):");
    gtk_label_set_xalign(GTK_LABEL(lbl_fps), 0.0);
    app.combo_fps = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_fps), "25");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_fps), "15");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_fps), "30");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_fps), 0);
    gtk_grid_attach(GTK_GRID(grid), lbl_fps, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.combo_fps, 1, row, 1, 1);
    row++;

    // 5. Bitrate H.264
    GtkWidget *lbl_bitrate = gtk_label_new("Bitrate H.264 (kbps):");
    gtk_label_set_xalign(GTK_LABEL(lbl_bitrate), 0.0);
    app.combo_bitrate = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_bitrate), "500");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_bitrate), "200");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_bitrate), "1500");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_bitrate), 0);
    gtk_grid_attach(GTK_GRID(grid), lbl_bitrate, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.combo_bitrate, 1, row, 1, 1);
    row++;

    // 6. Preset de Velocidade x264 (CPU)
    GtkWidget *lbl_preset = gtk_label_new("Velocidade do Encoder:");
    gtk_label_set_xalign(GTK_LABEL(lbl_preset), 0.0);
    app.combo_preset = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_preset), "ultrafast");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_preset), "fast");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_preset), "medium");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_preset), 0);
    gtk_grid_attach(GTK_GRID(grid), lbl_preset, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.combo_preset, 1, row, 1, 1);
    row++;

    // 7. Padrão de Animação de Teste
    GtkWidget *lbl_padrao = gtk_label_new("Padrão de Vídeo:");
    gtk_label_set_xalign(GTK_LABEL(lbl_padrao), 0.0);
    app.combo_padrao = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_padrao), "Bola Saltitante (ball)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_padrao), "Barras de Cores (smpte)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_padrao), "Ruído/Estática (snow)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_padrao), 0);
    gtk_grid_attach(GTK_GRID(grid), lbl_padrao, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.combo_padrao, 1, row, 1, 1);
    row++;

    // Separador
    gtk_box_pack_start(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    // Botões Iniciar e Interromper
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    app.btn_iniciar = gtk_button_new_with_label("Iniciar Transmissão");
    app.btn_interromper = gtk_button_new_with_label("Interromper");
    gtk_widget_set_sensitive(app.btn_interromper, FALSE);

    gtk_box_pack_start(GTK_BOX(btn_box), app.btn_iniciar, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(btn_box), app.btn_interromper, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), btn_box, FALSE, FALSE, 4);

    g_signal_connect(app.btn_iniciar, "clicked", G_CALLBACK(iniciar_transmissao), &app);
    g_signal_connect(app.btn_interromper, "clicked", G_CALLBACK(interromper_transmissao), &app);

    // Indicador de Estado
    app.lbl_status = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app.lbl_status), "<span foreground='#555555'>Status: <b>Parado</b></span>");
    gtk_box_pack_start(GTK_BOX(main_box), app.lbl_status, FALSE, FALSE, 4);

    gtk_widget_show_all(app.window);
    gtk_main();

    return 0;
}
