#include <gtk/gtk.h>
#include <adwaita.h>

#include "window.h"

int main(int argc, char **argv)
{
    AdwApplication *app =
        adw_application_new("dev.rhys.mcpe-launcher", G_APPLICATION_DEFAULT_FLAGS);
    app_context_attach(app);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}