# ipcam116 rtsp bridge

## general
* codebase is LLM assisted, you can easily tell whats been written by me and what hasnt

* directories
    * docs
        * some level of documentation of the protocol can be found here
    * tools
        * python debug tooling
        * vibecoded wireshark dissector (works well enough to unscramble the data)
    * src
        * rtsp bridge source code


## Building
* REQUIREMENTS
    * LIVE555: https://download.live555.com/
        * probably want no openssl, dont care
            * LINUX: `./genMakefiles linux-no-openssl`
        * windows version needs a bit of changes if i recall
            * TODO: add what needs to be done
    * CMake
    * Boost
        * on windows you have to compile boost, since this project makes use of 'program options'
    * C++ compiler that supports a little bit of C++23 (due to the use of `<print>`)


Windows. Open the x64 native tools command prompt for VS, then do something like this
```sh
cmake -B build -G "Visual Studio 17 2022" -A x64 -DBOOST_ROOT="C:/tools/libs/boost_1_88_0" -DLIVE555_ROOT="C:/tools/libs/live"
```

Linux, just point it to the root of LIVE555, go into build directory and run `make`
```sh
# probably something like this is enough
$ cmake -B build -DLIVE555_ROOT=/opt/live/
```

## usage
should be fairly easy
```
tutk_rtsp_bridge - bridges a TUTK/IOTC camera to RTSP

Usage: tutk_rtsp_bridge <uid> [options]
       tutk_rtsp_bridge --uid <uid> [options]

Options:
  -h [ --help ]                         print this help message and exit
  --uid arg                             camera's TUTK UID (required - can also
                                        be given positionally)
  --bind-ip arg (=0.0.0.0)              local interface to bind to (0.0.0.0 =
                                        all interfaces)
  --cam-user arg (=admin)               camera login username
  --cam-pass arg (=00000000)            camera login password - NOTE: visible
                                        in `ps`/shell history on this and every
                                        --*-pass option below; fine for a quick
                                        local run, not for anything shared
  --rtsp-port arg (=8554)               port the RTSP server listens on
  --rtsp-user arg (=none - RTSP server runs unauthenticated)
                                        RTSP auth username (leave unset for an
                                        open server - fine on a trusted LAN
                                        only)
  --rtsp-pass arg                       RTSP auth password (only meaningful if
                                        --rtsp-user is set)
  --handshake-timeout-ms arg (=5000)    overall deadline per handshake phase -
                                        see IOTCCameraConfig::handshakeTimeout
                                        for why this exists (UDP loss during
                                        the LAN search/knock sequence)
  --audio arg (=0)                      should we enable audio? can be fucky
  --log-level arg (=info)               debug|info|warn|error
```
if you have multiple interfaces, yes, you have to define the binding IP correctly yourself.

## known problems
* VLC timeline is jumping 2-3 seconds forwards then backwards
    * the cause is that audio frames dont contain a timestampMs unlike the video frames
    * at this point i am too lazy to figure it out, it seems to work mostly anyway, so i wouldn't worry that much
* Multiple RTSP consumers freezes the stream
    * yes, this is because of the bad Live555 setup i have going
        * honestly i would just throw this onto motionEye or something
        * or setup a restreamer to handle it properly