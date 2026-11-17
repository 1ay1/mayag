// bench/gtk4_app.cpp — GTK4 version of the comparison app.
//   gtk4_app [animate]
// A header and 200 two-line rows; "animate" moves a progress bar every frame.
#include <gtk/gtk.h>
#include <string>

static GtkWidget* bar;
static gboolean tick(GtkWidget*, GdkFrameClock* fc, gpointer) {
    double t = gdk_frame_clock_get_frame_time(fc) / 1e6;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), 0.5 + 0.5 * __builtin_sin(t * 3.0));
    return G_SOURCE_CONTINUE;
}

static void activate(GtkApplication* app, gpointer animate) {
    GtkWidget* win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "bench");
    gtk_window_set_default_size(GTK_WINDOW(win), 1000, 720);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget* title = gtk_label_new("Library");
    gtk_widget_add_css_class(title, "title-1");
    gtk_box_append(GTK_BOX(box), title);
    bar = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(box), bar);
    GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    for (int i = 0; i < 200; ++i) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_append(GTK_BOX(row), gtk_label_new(("Track " + std::to_string(i)).c_str()));
        gtk_box_append(GTK_BOX(row), gtk_label_new("Artist name"));
        gtk_box_append(GTK_BOX(list), row);
    }
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_window_set_child(GTK_WINDOW(win), box);
    if (animate) gtk_widget_add_tick_callback(bar, tick, nullptr, nullptr);
    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char** argv) {
    const bool animate = argc > 1 && std::string{argv[1]} == "animate";
    GtkApplication* app = gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), animate ? (gpointer)1 : nullptr);
    char* args[] = {argv[0], nullptr};
    return g_application_run(G_APPLICATION(app), 1, args);
}
