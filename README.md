# About

A tool for viewing and setting the bluetooth address
a sixaxis controller is currently paired with.


# Dependencies

Windows 11
VS 2022 Native tools cmd
`cl /EHsc /W4 /DUNICODE /D_UNICODE gui_sixaxispairer.c /link setupapi.lib hid.lib /SUBSYSTEM:WINDOWS`

# Supported Platforms

* Windows
* Mac
* Linux


# Building

    $ mkdir build
    $ cd build
    $ cmake ..
    $ make
    $ ./bin/sixaxispairer
    $ ./bin/sixaxispairer xx:xx:xx:xx:xx:xx
