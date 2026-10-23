#include "win32_pane_backend.h"
int main(){ auto f = it2app::makeWin32PaneBackendFactory({}); bool ok = it2app::shutdownPaneBackends(100); return (f && ok)?0:1; }
