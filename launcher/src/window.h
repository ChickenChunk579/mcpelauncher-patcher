#pragma once

#include <gtk/gtk.h>
#include <adwaita.h>

/* Attach the MCPE Launcher to an AdwApplication and run it on activation. */
void app_context_attach(AdwApplication *app);