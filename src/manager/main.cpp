#include "app.h"

#include "log.h"

#include <signal.h>

static app* g_app = nullptr;

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
    app instance;
    g_app = &instance;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (!instance.load(path))
    {
        LOG_ERR("usage: winject-manager <config>");
        return 1;
    }
    return instance.run();
}
