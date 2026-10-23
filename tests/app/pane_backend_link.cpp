#include "win32_pane_backend.h"
int main(){ auto f = it2app::makeWin32PaneBackendFactory({}); return f?0:1; }
