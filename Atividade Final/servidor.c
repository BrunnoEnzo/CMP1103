#include <gtk/gtk.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define DEFAULT_ARQUIVO "audio.mp3"
#define DEFAULT_IP "127.0.0.1"
#define DEFAULT_PORTA 5000

// Estrutura principal da aplicação Servidor (Transmissor de Áudio)
typedef struct {
    GtkWidget *window;
    GtkWidget *entry_ip;
    GtkWidget *spin_porta;
    GtkWidget *entry_arquivo;
    GtkWidget *btn_arquivo;
    GtkWidget *combo_amostragem;
    GtkWidget *combo_canais;
    GtkWidget *btn_iniciar;
    GtkWidget *btn_interromper;
    GtkWidget *progress_bar;
    GtkWidget *lbl_status;
    GtkWidget *lbl_ip_local;

    GstElement *pipeline;
    GstElement *audio_convert;
    guint bus_watch_id;
    guint timer_id;
    gboolean transmitindo;
    long tamanho_arquivo;
} ServidorAudioApp;

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

// Conexão dinâmica do pad decodificado do uridecodebin
static void pad_added_handler(GstElement *src, GstPad *new_pad, ServidorAudioApp *app) {
    (void)src;
    GstPad *sink_pad = gst_element_get_static_pad(app->audio_convert, "sink");
    if (!sink_pad) return;

    if (gst_pad_is_linked(sink_pad)) {
        gst_object_unref(sink_pad);
        return;
    }

    GstCaps *new_pad_caps = gst_pad_get_current_caps(new_pad);
    if (!new_pad_caps) {
        new_pad_caps = gst_pad_query_caps(new_pad, NULL);
    }

    if (new_pad_caps) {
        GstStructure *new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
        const gchar *new_pad_type = gst_structure_get_name(new_pad_struct);

        if (g_str_has_prefix(new_pad_type, "audio/x-raw")) {
            GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
            if (GST_PAD_LINK_FAILED(ret)) {
                g_printerr("Falha ao vincular pad de áudio do transmissor.\n");
            }
        }
        gst_caps_unref(new_pad_caps);
    }
    gst_object_unref(sink_pad);
}

// Interrompe a transmissão e restaura a interface
static void interromper_transmissao(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ServidorAudioApp *app = (ServidorAudioApp *)user_data;
    if (!app->transmitindo && !app->pipeline) return;

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
        app->audio_convert = NULL;
    }

    app->transmitindo = FALSE;
    gtk_widget_set_sensitive(app->btn_iniciar, TRUE);
    gtk_widget_set_sensitive(app->btn_interromper, FALSE);
    gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#555555'>Status: <b>Parado</b></span>");
}

// Atualização periódica do progresso da transmissão em MB
static gboolean atualizar_progresso(gpointer user_data) {
    ServidorAudioApp *app = (ServidorAudioApp *)user_data;
    if (!app->transmitindo || !app->pipeline) return FALSE;

    gint64 pos = 0, dur = 0;
    if (gst_element_query_position(app->pipeline, GST_FORMAT_TIME, &pos) &&
        gst_element_query_duration(app->pipeline, GST_FORMAT_TIME, &dur) && dur > 0) {
        double frac = (double)pos / (double)dur;
        if (frac > 1.0) frac = 1.0;
        if (frac < 0.0) frac = 0.0;

        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), frac);

        double mb_total = (double)app->tamanho_arquivo / (1024.0 * 1024.0);
        double mb_enviados = frac * mb_total;

        gchar *texto_barra = g_strdup_printf("%.2f MB / %.2f MB (%d%%)", mb_enviados, mb_total, (int)(frac * 100));
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress_bar), texto_barra);
        g_free(texto_barra);
    }
    return TRUE;
}

// Callback assíncrono do GstBus
static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer user_data) {
    (void)bus;
    ServidorAudioApp *app = (ServidorAudioApp *)user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("Erro na transmissão de áudio: %s\n", err->message);
            gchar *msg_err = g_strdup_printf("<span foreground='#c62828'><b>Erro: %s</b></span>", err->message);
            gtk_label_set_markup(GTK_LABEL(app->lbl_status), msg_err);
            g_free(msg_err);
            g_error_free(err);
            g_free(debug);
            interromper_transmissao(NULL, app);
            break;
        }
        case GST_MESSAGE_EOS:
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 1.0);
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress_bar), "100% Concluído");
            gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#2e7d32'>Status: <b>Transmissão de áudio concluída com sucesso (EOS)</b></span>");
            interromper_transmissao(NULL, app);
            break;
        default:
            break;
    }
    return TRUE;
}

// Inicia a pipeline de transmissão de áudio RTP/UDP
static void iniciar_transmissao(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ServidorAudioApp *app = (ServidorAudioApp *)user_data;
    if (app->transmitindo) return;

    const gchar *ip = gtk_entry_get_text(GTK_ENTRY(app->entry_ip));
    if (!ip || strlen(ip) == 0) ip = DEFAULT_IP;
    int porta = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_porta));

    const gchar *arquivo = gtk_entry_get_text(GTK_ENTRY(app->entry_arquivo));
    if (!arquivo || strlen(arquivo) == 0) arquivo = DEFAULT_ARQUIVO;

    // Validação da existência e tamanho do arquivo
    FILE *f = fopen(arquivo, "rb");
    if (!f) {
        gchar *err_txt = g_strdup_printf("<span foreground='#c62828'><b>Arquivo '%s' não encontrado!</b></span>", arquivo);
        gtk_label_set_markup(GTK_LABEL(app->lbl_status), err_txt);
        g_free(err_txt);
        return;
    }
    fseek(f, 0, SEEK_END);
    app->tamanho_arquivo = ftell(f);
    fclose(f);

    if (app->tamanho_arquivo <= 0) {
        gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#c62828'><b>O arquivo de áudio está vazio!</b></span>");
        return;
    }

    // Parâmetros de áudio configurados na interface
    const gchar *amostragem_str = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app->combo_amostragem));
    int rate = 44100;
    if (amostragem_str) {
        if (g_str_has_prefix(amostragem_str, "48000")) rate = 48000;
        else if (g_str_has_prefix(amostragem_str, "22050")) rate = 22050;
    }

    const gchar *canais_str = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app->combo_canais));
    int canais = (canais_str && g_str_has_prefix(canais_str, "1")) ? 1 : 2;

    // Criação dos elementos da pipeline de áudio (reutilizando arquitetura da atividade)
    GstElement *source = gst_element_factory_make("uridecodebin", "source");
    app->audio_convert = gst_element_factory_make("audioconvert", "convert");
    GstElement *resample = gst_element_factory_make("audioresample", "resample");
    GstElement *filter = gst_element_factory_make("capsfilter", "filter");
    GstElement *payloader = gst_element_factory_make("rtpL16pay", "payloader");
    GstElement *sink = gst_element_factory_make("udpsink", "sink");
    app->pipeline = gst_pipeline_new("pipeline-servidor-audio");

    if (!app->pipeline || !source || !app->audio_convert || !resample || !filter || !payloader || !sink) {
        gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#c62828'><b>Falha ao criar elementos GStreamer!</b></span>");
        if (app->pipeline) { gst_object_unref(app->pipeline); app->pipeline = NULL; }
        return;
    }

    // Configuração do arquivo de origem via URI
    gchar *uri = gst_filename_to_uri(arquivo, NULL);
    g_object_set(source, "uri", uri, NULL);
    g_free(uri);

    // Caps de áudio linear PCM S16BE
    gchar *caps_str = g_strdup_printf("audio/x-raw, format=S16BE, channels=%d, rate=%d", canais, rate);
    GstCaps *caps = gst_caps_from_string(caps_str);
    g_object_set(filter, "caps", caps, NULL);
    gst_caps_unref(caps);
    g_free(caps_str);

    // Configuração de destino UDP com sincronização em tempo real
    g_object_set(sink, "host", ip, "port", porta, "sync", TRUE, NULL);

    // Montagem e ligação da pipeline
    gst_bin_add_many(GST_BIN(app->pipeline), source, app->audio_convert, resample, filter, payloader, sink, NULL);
    if (!gst_element_link_many(app->audio_convert, resample, filter, payloader, sink, NULL)) {
        gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#c62828'><b>Erro ao ligar elementos da pipeline!</b></span>");
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
        return;
    }

    g_signal_connect(source, "pad-added", G_CALLBACK(pad_added_handler), app);

    // Barramento de mensagens
    GstBus *bus = gst_element_get_bus(app->pipeline);
    app->bus_watch_id = gst_bus_add_watch(bus, bus_callback, app);
    gst_object_unref(bus);

    // Início da transmissão
    GstStateChangeReturn ret = gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        gtk_label_set_markup(GTK_LABEL(app->lbl_status), "<span foreground='#c62828'><b>Erro ao iniciar estado PLAYING!</b></span>");
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
        return;
    }

    app->transmitindo = TRUE;
    gtk_widget_set_sensitive(app->btn_iniciar, FALSE);
    gtk_widget_set_sensitive(app->btn_interromper, TRUE);

    // Timer para atualização suave da barra de progresso (a cada 200ms)
    app->timer_id = g_timeout_add(200, atualizar_progresso, app);

    gchar *status_txt = g_strdup_printf(
        "<span foreground='#2e7d32'><b>Transmitindo áudio para %s:%d</b> [%d Hz, %s]</span>",
        ip, porta, rate, (canais == 2) ? "Estéreo" : "Mono"
    );
    gtk_label_set_markup(GTK_LABEL(app->lbl_status), status_txt);
    g_free(status_txt);
}

// Diálogo para escolha do arquivo de áudio
static void selecionar_arquivo(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ServidorAudioApp *app = (ServidorAudioApp *)user_data;
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Selecionar Arquivo de Áudio",
        GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancelar", GTK_RESPONSE_CANCEL,
        "Abrir", GTK_RESPONSE_ACCEPT,
        NULL
    );

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Arquivos de Áudio (*.mp3, *.wav, *.ogg, *.flac)");
    gtk_file_filter_add_pattern(filter, "*.mp3");
    gtk_file_filter_add_pattern(filter, "*.wav");
    gtk_file_filter_add_pattern(filter, "*.ogg");
    gtk_file_filter_add_pattern(filter, "*.flac");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        gtk_entry_set_text(GTK_ENTRY(app->entry_arquivo), filename);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

// Encerramento limpo ao fechar a janela
static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    ServidorAudioApp *app = (ServidorAudioApp *)user_data;
    interromper_transmissao(NULL, app);
    gtk_main_quit();
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    gtk_init(&argc, &argv);

    ServidorAudioApp app;
    memset(&app, 0, sizeof(ServidorAudioApp));

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Servidor Multimídia — Transmissor de Áudio RTP/UDP");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 540, 480);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    gtk_window_set_keep_above(GTK_WINDOW(app.window), TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(app.window), 16);
    g_signal_connect(app.window, "destroy", G_CALLBACK(on_window_destroy), &app);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_add(GTK_CONTAINER(app.window), main_box);

    // Cabeçalho
    GtkWidget *lbl_header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_header),
        "<span size='x-large' weight='bold' foreground='#17365D'>Servidor de Streaming de Áudio</span>\n"
        "<span size='small' foreground='#555555'>Transmissão RTP L16 / UDP em Tempo Real</span>");
    gtk_label_set_xalign(GTK_LABEL(lbl_header), 0.0);
    gtk_box_pack_start(GTK_BOX(main_box), lbl_header, FALSE, FALSE, 0);

    // IP local informativo
    gchar *ip_local = obter_ips_locais();
    gchar *lbl_ip_text = g_strdup_printf("<span size='small' foreground='#3973AC'>IP desta máquina (Transmissor): <b>%s</b></span>", ip_local);
    app.lbl_ip_local = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app.lbl_ip_local), lbl_ip_text);
    gtk_label_set_xalign(GTK_LABEL(app.lbl_ip_local), 0.0);
    gtk_box_pack_start(GTK_BOX(main_box), app.lbl_ip_local, FALSE, FALSE, 0);
    g_free(ip_local);
    g_free(lbl_ip_text);

    gtk_box_pack_start(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    // Grid de configurações
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_pack_start(GTK_BOX(main_box), grid, FALSE, FALSE, 0);

    int row = 0;

    // 1. IP de destino
    GtkWidget *lbl_dest_ip = gtk_label_new("IP do Receptor:");
    gtk_label_set_xalign(GTK_LABEL(lbl_dest_ip), 0.0);
    app.entry_ip = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(app.entry_ip), DEFAULT_IP);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.entry_ip), "ex: 192.168.1.50 ou 127.0.0.1");
    gtk_grid_attach(GTK_GRID(grid), lbl_dest_ip, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.entry_ip, 1, row, 2, 1);
    row++;

    // 2. Porta UDP
    GtkWidget *lbl_dest_porta = gtk_label_new("Porta UDP:");
    gtk_label_set_xalign(GTK_LABEL(lbl_dest_porta), 0.0);
    app.spin_porta = gtk_spin_button_new_with_range(1024, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.spin_porta), DEFAULT_PORTA);
    gtk_grid_attach(GTK_GRID(grid), lbl_dest_porta, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.spin_porta, 1, row, 2, 1);
    row++;

    // 3. Arquivo de áudio
    GtkWidget *lbl_arq = gtk_label_new("Arquivo de Áudio:");
    gtk_label_set_xalign(GTK_LABEL(lbl_arq), 0.0);
    app.entry_arquivo = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(app.entry_arquivo), DEFAULT_ARQUIVO);
    app.btn_arquivo = gtk_button_new_with_label("Buscar...");
    g_signal_connect(app.btn_arquivo, "clicked", G_CALLBACK(selecionar_arquivo), &app);

    GtkWidget *box_arq = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(box_arq), app.entry_arquivo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box_arq), app.btn_arquivo, FALSE, FALSE, 0);

    gtk_grid_attach(GTK_GRID(grid), lbl_arq, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), box_arq, 1, row, 2, 1);
    row++;

    // 4. Taxa de Amostragem (Qualidade)
    GtkWidget *lbl_taxa = gtk_label_new("Taxa de Amostragem:");
    gtk_label_set_xalign(GTK_LABEL(lbl_taxa), 0.0);
    app.combo_amostragem = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_amostragem), "44100 Hz (Qualidade CD - Padrão)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_amostragem), "48000 Hz (Estúdio / DAT)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_amostragem), "22050 Hz (Baixa Banda)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_amostragem), 0);
    gtk_grid_attach(GTK_GRID(grid), lbl_taxa, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.combo_amostragem, 1, row, 2, 1);
    row++;

    // 5. Canais de Áudio
    GtkWidget *lbl_canais = gtk_label_new("Canais:");
    gtk_label_set_xalign(GTK_LABEL(lbl_canais), 0.0);
    app.combo_canais = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_canais), "2 Canais (Estéreo - Padrão)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_canais), "1 Canal (Mono)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_canais), 0);
    gtk_grid_attach(GTK_GRID(grid), lbl_canais, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.combo_canais, 1, row, 2, 1);
    row++;

    gtk_box_pack_start(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    // Barra de Progresso
    GtkWidget *lbl_prog = gtk_label_new("Progresso da Transmissão (em MB):");
    gtk_label_set_xalign(GTK_LABEL(lbl_prog), 0.0);
    gtk_box_pack_start(GTK_BOX(main_box), lbl_prog, FALSE, FALSE, 0);

    app.progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app.progress_bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app.progress_bar), "0.00 MB / 0.00 MB (0%)");
    gtk_box_pack_start(GTK_BOX(main_box), app.progress_bar, FALSE, FALSE, 2);

    // Botões
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

    gboolean autostart = FALSE;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--autostart") == 0 || strcmp(argv[i], "-a") == 0) {
            autostart = TRUE;
        } else if (strncmp(argv[i], "--ip=", 5) == 0) {
            gtk_entry_set_text(GTK_ENTRY(app.entry_ip), argv[i] + 5);
        } else if (argv[i][0] != '-') {
            gtk_entry_set_text(GTK_ENTRY(app.entry_ip), argv[i]);
        }
    }

    gtk_widget_show_all(app.window);

    if (autostart) {
        iniciar_transmissao(NULL, &app);
    }

    gtk_main();

    return 0;
}
