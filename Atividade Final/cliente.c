#include <gtk/gtk.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define DEFAULT_PORTA 5000
#define DEFAULT_LATENCIA 200

// Estrutura principal da aplicação Cliente (Receptor de Áudio)
typedef struct {
    GtkWidget *window;
    GtkWidget *spin_porta;
    GtkWidget *combo_latencia;
    GtkWidget *combo_amostragem;
    GtkWidget *combo_canais;
    GtkWidget *btn_iniciar;
    GtkWidget *btn_interromper;
    GtkWidget *lbl_status;
    GtkWidget *lbl_ip_local;
    GtkWidget *progress_audio;

    // Métricas do rtpjitterbuffer
    GtkWidget *lbl_pushed;
    GtkWidget *lbl_lost;
    GtkWidget *lbl_late;
    GtkWidget *lbl_duplicates;
    GtkWidget *lbl_jitter;

    GstElement *pipeline;
    GstElement *jitterbuffer;
    guint bus_watch_id;
    guint timer_id;
    gboolean recebendo;

    int porta_configurada;
    int latencia_configurada;
    int taxa_amostragem;
    int canais;

    guint64 pushed;
    guint64 lost;
    guint64 late;
    guint64 duplicates;
    guint64 jitter;
} ClienteAudioApp;

// Detecta e retorna os endereços IPv4 das interfaces ativas do receptor
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

// Criação de sink de áudio compatível para evitar falhas de driver/PipeWire
static GstElement *criar_audio_sink(void) {
    GstElement *sink = NULL;
    const char *sinks[] = {"alsasink", "pulsesink", "autoaudiosink", NULL};

    for (int i = 0; sinks[i] != NULL; i++) {
        sink = gst_element_factory_make(sinks[i], "sink");
        if (sink) {
            if (gst_element_set_state(sink, GST_STATE_READY) == GST_STATE_CHANGE_SUCCESS) {
                gst_element_set_state(sink, GST_STATE_NULL);
                return sink;
            }
            gst_object_unref(sink);
            sink = NULL;
        }
    }
    return gst_element_factory_make("autoaudiosink", "sink");
}

// Salva o relatório consolidado de métricas em arquivo de texto
static void salvar_relatorio_log(ClienteAudioApp *app) {
    if (app->pushed == 0 && app->lost == 0) return;

    char nome_log[128];
    snprintf(nome_log, sizeof(nome_log), "log_receptor_%dms.txt", app->latencia_configurada);

    const char *arquivos_log[] = {nome_log, "log_receptor.txt", NULL};
    time_t agora = time(NULL);

    for (int i = 0; arquivos_log[i] != NULL; i++) {
        FILE *log = fopen(arquivos_log[i], "w");
        if (log) {
            fprintf(log, "=== RELATÓRIO DE STREAMING RTP/UDP (CLIENTE GTK 3) ===\n");
            fprintf(log, "Data/Hora: %s", ctime(&agora));
            fprintf(log, "Porta UDP: %d\n", app->porta_configurada);
            fprintf(log, "Configuração de Latência (rtpjitterbuffer): %d ms\n", app->latencia_configurada);
            fprintf(log, "Taxa de Amostragem: %d Hz | Canais: %d\n", app->taxa_amostragem, app->canais);
            fprintf(log, "Modo de reprodução: Streaming de áudio em tempo real no alto-falante\n");
            fprintf(log, "Pacotes recebidos (num-pushed): %lu\n", (unsigned long)app->pushed);
            fprintf(log, "Pacotes perdidos (num-lost): %lu\n", (unsigned long)app->lost);
            fprintf(log, "Pacotes atrasados (num-late): %lu\n", (unsigned long)app->late);
            fprintf(log, "Pacotes duplicados: %lu\n", (unsigned long)app->duplicates);
            fprintf(log, "Jitter médio calculado: %lu ns (%.2f ms)\n", (unsigned long)app->jitter, (double)app->jitter / 1000000.0);
            fprintf(log, "Status final: Transmissão recebida e encerrada com sucesso\n");
            fclose(log);
        }
    }
}

// Interrompe a recepção de áudio e atualiza a interface
static void interromper_recepcao(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ClienteAudioApp *app = (ClienteAudioApp *)user_data;
    if (!app->recebendo && !app->pipeline) return;

    if (app->timer_id > 0) {
        g_source_remove(app->timer_id);
        app->timer_id = 0;
    }

    if (app->bus_watch_id > 0) {
        g_source_remove(app->bus_watch_id);
        app->bus_watch_id = 0;
    }

    if (app->pipeline) {
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
        app->jitterbuffer = NULL;
    }

    salvar_relatorio_log(app);

    app->recebendo = FALSE;
    gtk_widget_set_sensitive(app->btn_iniciar, TRUE);
    gtk_widget_set_sensitive(app->btn_interromper, FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_audio), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress_audio), "Parado");
    gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#555555'>Status: <b>Recepção finalizada</b> (Relatório salvo em log_receptor.txt)</span>");
}

// Atualização periódica da telemetria a partir do rtpjitterbuffer
static gboolean atualizar_telemetria(gpointer user_data) {
    ClienteAudioApp *app = (ClienteAudioApp *)user_data;
    if (!app->recebendo || !app->jitterbuffer) return FALSE;

    GstStructure *stats = NULL;
    g_object_get(app->jitterbuffer, "stats", &stats, NULL);
    if (stats) {
        gst_structure_get_uint64(stats, "num-pushed", &app->pushed);
        gst_structure_get_uint64(stats, "num-lost", &app->lost);
        gst_structure_get_uint64(stats, "num-late", &app->late);
        gst_structure_get_uint64(stats, "num-duplicates", &app->duplicates);
        gst_structure_get_uint64(stats, "avg-jitter", &app->jitter);
        gst_structure_free(stats);

        gchar *txt = g_strdup_printf("%lu", (unsigned long)app->pushed);
        gtk_label_set_text(GTK_LABEL(app->lbl_pushed), txt);
        g_free(txt);

        txt = g_strdup_printf("%lu", (unsigned long)app->lost);
        gtk_label_set_text(GTK_LABEL(app->lbl_lost), txt);
        g_free(txt);

        txt = g_strdup_printf("%lu", (unsigned long)app->late);
        gtk_label_set_text(GTK_LABEL(app->lbl_late), txt);
        g_free(txt);

        txt = g_strdup_printf("%lu", (unsigned long)app->duplicates);
        gtk_label_set_text(GTK_LABEL(app->lbl_duplicates), txt);
        g_free(txt);

        txt = g_strdup_printf("%.2f ms (%lu ns)", (double)app->jitter / 1000000.0, (unsigned long)app->jitter);
        gtk_label_set_text(GTK_LABEL(app->lbl_jitter), txt);
        g_free(txt);

        if (app->pushed > 0) {
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(app->progress_audio));
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress_audio), "Áudio em Reprodução (Recebendo)");
            gchar *msg_status = g_strdup_printf(
                "<span foreground='#2e7d32'>Status: <b>Recebendo áudio na porta %d</b> (%lu pacotes)</span>",
                app->porta_configurada, (unsigned long)app->pushed
            );
            gtk_label_set_markup(GTK_LABEL(app->lbl_status), msg_status);
            g_free(msg_status);
        }
    }
    return TRUE;
}

// Callback assíncrono do GstBus para tratamento de mensagens no loop GLib
static gboolean bus_callback_cliente(GstBus *bus, GstMessage *msg, gpointer user_data) {
    (void)bus;
    ClienteAudioApp *app = (ClienteAudioApp *)user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("Erro no receptor de áudio: %s\n", err->message);
            gchar *msg_err = g_strdup_printf("<span foreground='#c62828'><b>Erro na recepção: %s</b></span>", err->message);
            gtk_label_set_markup(GTK_LABEL(app->lbl_status), msg_err);
            g_free(msg_err);
            g_error_free(err);
            g_free(debug);
            interromper_recepcao(NULL, app);
            break;
        }
        case GST_MESSAGE_EOS:
            gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#2e7d32'>Status: <b>Fim do fluxo de áudio (EOS)</b></span>");
            interromper_recepcao(NULL, app);
            break;
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *s = gst_message_get_structure(msg);
            if (gst_structure_has_name(s, "GstUDPSrcTimeout")) {
                if (app->pushed > 0) {
                    gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#555555'>Status: <b>Transmissão finalizada pelo servidor (Timeout)</b></span>");
                    interromper_recepcao(NULL, app);
                }
            }
            break;
        }
        default:
            break;
    }
    return TRUE;
}

// Inicia a pipeline de recepção e reprodução ao vivo de áudio RTP/UDP
static void iniciar_recepcao(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ClienteAudioApp *app = (ClienteAudioApp *)user_data;
    if (app->recebendo) return;

    app->porta_configurada = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_porta));

    // Latência do jitter buffer selecionada
    const gchar *lat_str = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app->combo_latencia));
    app->latencia_configurada = DEFAULT_LATENCIA;
    if (lat_str) {
        if (g_str_has_prefix(lat_str, "50")) app->latencia_configurada = 50;
        else if (g_str_has_prefix(lat_str, "100")) app->latencia_configurada = 100;
        else if (g_str_has_prefix(lat_str, "200")) app->latencia_configurada = 200;
        else if (g_str_has_prefix(lat_str, "400")) app->latencia_configurada = 400;
    }

    // Taxa de amostragem
    const gchar *rate_str = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app->combo_amostragem));
    app->taxa_amostragem = 44100;
    if (rate_str) {
        if (g_str_has_prefix(rate_str, "48000")) app->taxa_amostragem = 48000;
        else if (g_str_has_prefix(rate_str, "22050")) app->taxa_amostragem = 22050;
    }

    // Canais
    const gchar *canais_str = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app->combo_canais));
    app->canais = (canais_str && g_str_has_prefix(canais_str, "1")) ? 1 : 2;

    // Reset dos contadores
    app->pushed = 0;
    app->lost = 0;
    app->late = 0;
    app->duplicates = 0;
    app->jitter = 0;

    gtk_label_set_text(GTK_LABEL(app->lbl_pushed), "0");
    gtk_label_set_text(GTK_LABEL(app->lbl_lost), "0");
    gtk_label_set_text(GTK_LABEL(app->lbl_late), "0");
    gtk_label_set_text(GTK_LABEL(app->lbl_duplicates), "0");
    gtk_label_set_text(GTK_LABEL(app->lbl_jitter), "0.00 ms");

    // Criação dos elementos da pipeline de recepção
    GstElement *src = gst_element_factory_make("udpsrc", "udpsrc");
    app->jitterbuffer = gst_element_factory_make("rtpjitterbuffer", "jitterbuffer");
    GstElement *depayloader = gst_element_factory_make("rtpL16depay", "depayloader");
    GstElement *convert = gst_element_factory_make("audioconvert", "convert");
    GstElement *resample = gst_element_factory_make("audioresample", "resample");
    GstElement *sink = criar_audio_sink();
    app->pipeline = gst_pipeline_new("pipeline-cliente-audio");

    if (!app->pipeline || !src || !app->jitterbuffer || !depayloader || !convert || !resample || !sink) {
        gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#c62828'><b>Falha ao criar elementos GStreamer do cliente!</b></span>");
        if (app->pipeline) { gst_object_unref(app->pipeline); app->pipeline = NULL; }
        return;
    }

    // Caps de recepção RTP L16 compatíveis com o transmissor
    gchar *caps_str = g_strdup_printf(
        "application/x-rtp, media=audio, clock-rate=%d, encoding-name=L16, channels=(int)%d, payload=96",
        app->taxa_amostragem, app->canais
    );
    GstCaps *caps = gst_caps_from_string(caps_str);
    g_object_set(src, "port", app->porta_configurada, "address", "0.0.0.0", "caps", caps, "timeout", (guint64)3000000000ULL, NULL);
    gst_caps_unref(caps);
    g_free(caps_str);

    // Configuração da latência do buffer contra oscilações de rede (jitter)
    g_object_set(app->jitterbuffer, "latency", app->latencia_configurada, "do-lost", TRUE, NULL);

    // Montagem e ligação da pipeline
    gst_bin_add_many(GST_BIN(app->pipeline), src, app->jitterbuffer, depayloader, convert, resample, sink, NULL);
    if (!gst_element_link_many(src, app->jitterbuffer, depayloader, convert, resample, sink, NULL)) {
        gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#c62828'><b>Falha ao vincular elementos do cliente!</b></span>");
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
        return;
    }

    // Barramento de mensagens assíncrono
    GstBus *bus = gst_element_get_bus(app->pipeline);
    app->bus_watch_id = gst_bus_add_watch(bus, bus_callback_cliente, app);
    gst_object_unref(bus);

    // Início da recepção
    GstStateChangeReturn ret = gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#c62828'><b>Erro ao iniciar pipeline do receptor!</b></span>");
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
        return;
    }

    app->recebendo = TRUE;
    gtk_widget_set_sensitive(app->btn_iniciar, FALSE);
    gtk_widget_set_sensitive(app->btn_interromper, TRUE);

    // Atualização em tempo real das métricas a cada 250ms
    app->timer_id = g_timeout_add(250, atualizar_telemetria, app);

    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress_audio), "Aguardando fluxo de áudio...");

    gchar *status_txt = g_strdup_printf(
        "<span foreground='#17365D'><b>Aguardando áudio na porta UDP %d</b> (Jitter Buffer: %d ms)...</span>",
        app->porta_configurada, app->latencia_configurada
    );
    gtk_label_set_markup(GTK_LABEL(app->lbl_status), status_txt);
    g_free(status_txt);
}

// Fechamento limpo da janela
static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    ClienteAudioApp *app = (ClienteAudioApp *)user_data;
    interromper_recepcao(NULL, app);
    gtk_main_quit();
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    gtk_init(&argc, &argv);

    ClienteAudioApp app;
    memset(&app, 0, sizeof(ClienteAudioApp));

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Cliente Multimídia — Receptor de Áudio RTP/UDP");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 540, 520);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(app.window), 16);
    g_signal_connect(app.window, "destroy", G_CALLBACK(on_window_destroy), &app);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_add(GTK_CONTAINER(app.window), main_box);

    // Cabeçalho da aplicação
    GtkWidget *lbl_header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_header),
        "<span size='x-large' weight='bold' foreground='#17365D'>Cliente de Streaming de Áudio</span>\n"
        "<span size='small' foreground='#555555'>Recepção RTP L16 / UDP em Tempo Real com Jitter Buffer</span>");
    gtk_label_set_xalign(GTK_LABEL(lbl_header), 0.0);
    gtk_box_pack_start(GTK_BOX(main_box), lbl_header, FALSE, FALSE, 0);

    // Frame de Configuração de Rede e Áudio
    GtkWidget *frame_cfg = gtk_frame_new("Configurações do Receptor");
    gtk_box_pack_start(GTK_BOX(main_box), frame_cfg, FALSE, FALSE, 0);

    GtkWidget *grid_cfg = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_cfg), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid_cfg), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid_cfg), 12);
    gtk_container_add(GTK_CONTAINER(frame_cfg), grid_cfg);

    // Porta UDP
    GtkWidget *lbl_porta = gtk_label_new("Porta UDP:");
    gtk_label_set_xalign(GTK_LABEL(lbl_porta), 0.0);
    gtk_grid_attach(GTK_GRID(grid_cfg), lbl_porta, 0, 0, 1, 1);

    app.spin_porta = gtk_spin_button_new_with_range(1024, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.spin_porta), DEFAULT_PORTA);
    gtk_widget_set_hexpand(app.spin_porta, TRUE);
    gtk_grid_attach(GTK_GRID(grid_cfg), app.spin_porta, 1, 0, 1, 1);

    // Latência do Jitter Buffer
    GtkWidget *lbl_lat = gtk_label_new("Jitter Buffer:");
    gtk_label_set_xalign(GTK_LABEL(lbl_lat), 0.0);
    gtk_grid_attach(GTK_GRID(grid_cfg), lbl_lat, 0, 1, 1, 1);

    app.combo_latencia = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_latencia), "50 ms (Baixa latência)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_latencia), "100 ms (Equilibrado)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_latencia), "200 ms (Padrão recomendado)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_latencia), "400 ms (Alta tolerância a jitter)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_latencia), 2);
    gtk_grid_attach(GTK_GRID(grid_cfg), app.combo_latencia, 1, 1, 1, 1);

    // Taxa de Amostragem
    GtkWidget *lbl_rate = gtk_label_new("Taxa de Amostragem:");
    gtk_label_set_xalign(GTK_LABEL(lbl_rate), 0.0);
    gtk_grid_attach(GTK_GRID(grid_cfg), lbl_rate, 0, 2, 1, 1);

    app.combo_amostragem = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_amostragem), "44100 Hz (Padrão CD)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_amostragem), "48000 Hz (Alta fidelidade)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_amostragem), "22050 Hz (Econômico)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_amostragem), 0);
    gtk_grid_attach(GTK_GRID(grid_cfg), app.combo_amostragem, 1, 2, 1, 1);

    // Canais
    GtkWidget *lbl_canais = gtk_label_new("Canais:");
    gtk_label_set_xalign(GTK_LABEL(lbl_canais), 0.0);
    gtk_grid_attach(GTK_GRID(grid_cfg), lbl_canais, 0, 3, 1, 1);

    app.combo_canais = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_canais), "2 Canais (Estéreo)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_canais), "1 Canal (Mono)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_canais), 0);
    gtk_grid_attach(GTK_GRID(grid_cfg), app.combo_canais, 1, 3, 1, 1);

    // IP local informativo
    gchar *ip_info = obter_ips_locais();
    gchar *markup_ip = g_strdup_printf(
        "<span size='small' foreground='#1b5e20'><b>Seu IP para o transmissor:</b> %s</span>",
        ip_info
    );
    app.lbl_ip_local = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app.lbl_ip_local), markup_ip);
    gtk_label_set_xalign(GTK_LABEL(app.lbl_ip_local), 0.0);
    gtk_grid_attach(GTK_GRID(grid_cfg), app.lbl_ip_local, 0, 4, 2, 1);
    g_free(markup_ip);
    g_free(ip_info);

    // Frame de Telemetria e Estatísticas de Rede
    GtkWidget *frame_stats = gtk_frame_new("Telemetria e Qualidade em Tempo Real");
    gtk_box_pack_start(GTK_BOX(main_box), frame_stats, TRUE, TRUE, 0);

    GtkWidget *grid_stats = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_stats), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid_stats), 16);
    gtk_container_set_border_width(GTK_CONTAINER(grid_stats), 12);
    gtk_container_add(GTK_CONTAINER(frame_stats), grid_stats);

    // Linha 0: Pacotes Recebidos
    GtkWidget *lbl_t_pushed = gtk_label_new("Pacotes Recebidos (pushed):");
    gtk_label_set_xalign(GTK_LABEL(lbl_t_pushed), 0.0);
    gtk_grid_attach(GTK_GRID(grid_stats), lbl_t_pushed, 0, 0, 1, 1);
    app.lbl_pushed = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(app.lbl_pushed), 1.0);
    gtk_grid_attach(GTK_GRID(grid_stats), app.lbl_pushed, 1, 0, 1, 1);

    // Linha 1: Pacotes Perdidos
    GtkWidget *lbl_t_lost = gtk_label_new("Pacotes Perdidos (lost):");
    gtk_label_set_xalign(GTK_LABEL(lbl_t_lost), 0.0);
    gtk_grid_attach(GTK_GRID(grid_stats), lbl_t_lost, 0, 1, 1, 1);
    app.lbl_lost = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(app.lbl_lost), 1.0);
    gtk_grid_attach(GTK_GRID(grid_stats), app.lbl_lost, 1, 1, 1, 1);

    // Linha 2: Pacotes Atrasados
    GtkWidget *lbl_t_late = gtk_label_new("Pacotes Atrasados (late):");
    gtk_label_set_xalign(GTK_LABEL(lbl_t_late), 0.0);
    gtk_grid_attach(GTK_GRID(grid_stats), lbl_t_late, 0, 2, 1, 1);
    app.lbl_late = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(app.lbl_late), 1.0);
    gtk_grid_attach(GTK_GRID(grid_stats), app.lbl_late, 1, 2, 1, 1);

    // Linha 3: Pacotes Duplicados
    GtkWidget *lbl_t_dup = gtk_label_new("Pacotes Duplicados:");
    gtk_label_set_xalign(GTK_LABEL(lbl_t_dup), 0.0);
    gtk_grid_attach(GTK_GRID(grid_stats), lbl_t_dup, 0, 3, 1, 1);
    app.lbl_duplicates = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(app.lbl_duplicates), 1.0);
    gtk_grid_attach(GTK_GRID(grid_stats), app.lbl_duplicates, 1, 3, 1, 1);

    // Linha 4: Jitter Médio
    GtkWidget *lbl_t_jit = gtk_label_new("Jitter Médio Calculado:");
    gtk_label_set_xalign(GTK_LABEL(lbl_t_jit), 0.0);
    gtk_grid_attach(GTK_GRID(grid_stats), lbl_t_jit, 0, 4, 1, 1);
    app.lbl_jitter = gtk_label_new("0.00 ms");
    gtk_label_set_xalign(GTK_LABEL(app.lbl_jitter), 1.0);
    gtk_grid_attach(GTK_GRID(grid_stats), app.lbl_jitter, 1, 4, 1, 1);

    // Linha 5: Atividade Sonora
    GtkWidget *lbl_t_act = gtk_label_new("Atividade de Áudio:");
    gtk_label_set_xalign(GTK_LABEL(lbl_t_act), 0.0);
    gtk_grid_attach(GTK_GRID(grid_stats), lbl_t_act, 0, 5, 1, 1);
    app.progress_audio = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app.progress_audio), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app.progress_audio), "Parado");
    gtk_widget_set_hexpand(app.progress_audio, TRUE);
    gtk_grid_attach(GTK_GRID(grid_stats), app.progress_audio, 1, 5, 1, 1);

    // Linha de Status
    app.lbl_status = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app.lbl_status), "<span foreground='#555555'>Status: <b>Pronto para receber</b></span>");
    gtk_label_set_xalign(GTK_LABEL(app.lbl_status), 0.0);
    gtk_box_pack_start(GTK_BOX(main_box), app.lbl_status, FALSE, FALSE, 0);

    // Botões de Ação
    GtkWidget *box_botoes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(main_box), box_botoes, FALSE, FALSE, 0);

    app.btn_iniciar = gtk_button_new_with_label("Iniciar Recepção");
    gtk_widget_set_hexpand(app.btn_iniciar, TRUE);
    g_signal_connect(app.btn_iniciar, "clicked", G_CALLBACK(iniciar_recepcao), &app);
    gtk_box_pack_start(GTK_BOX(box_botoes), app.btn_iniciar, TRUE, TRUE, 0);

    app.btn_interromper = gtk_button_new_with_label("Interromper Recepção");
    gtk_widget_set_sensitive(app.btn_interromper, FALSE);
    gtk_widget_set_hexpand(app.btn_interromper, TRUE);
    g_signal_connect(app.btn_interromper, "clicked", G_CALLBACK(interromper_recepcao), &app);
    gtk_box_pack_start(GTK_BOX(box_botoes), app.btn_interromper, TRUE, TRUE, 0);

    gtk_widget_show_all(app.window);
    gtk_main();

    return 0;
}
