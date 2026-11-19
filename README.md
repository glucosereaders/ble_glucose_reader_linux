Bluetooth LE application for linux to read glucose measurements from sensors / meters supporting the Bluetooth Glucose Profile (https://www.bluetooth.com/specifications/specs/glp-1-0-1/).

Application reads from meter and stores measurements to file.

Written in C++20, using dbus-cxx 2 (https://github.com/dbus-cxx/dbus-cxx).
Communicates with Bluez / bluetoothd.

Built with CMake.

Tested on:
- Ascensia Contour Next
- Roche Accu-Chek Guide Me
