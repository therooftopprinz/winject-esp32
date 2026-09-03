#include "app.h"

#include "log.h"

#include <signal.h>

static App* g_app = nullptr;

static void on_signal(int)
{
    if (g_app != nullptr)
    {
        g_app->stop();
    }
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "winject.conf";
    signal(SIGPIPE, SIG_IGN);
    App app;
    g_app = &app;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (!app.load(path))
    {
        LOG_ERR("usage: winject-manager <config>");
        return 1;
    }
    return app.run();
}
