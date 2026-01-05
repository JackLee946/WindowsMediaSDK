#include <iostream>

#include "main_window.h"
#include "minidump.h"

int main(void) {
    InstallMiniDumpHandler();
    MainWindow main_window;
    main_window.Init();
    main_window.CreateDuiWindow();
    main_window.Show();
    return 0;
}


