#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "MicroDriveControl.h"
#include "UserInterface.h"

int main()
{
    //Start the user interface in core1
    multicore_launch_core1(RunUserInterface);

    //Run the MD control in core0
    RunMDControl();

    return 0;
}
