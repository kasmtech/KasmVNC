# WebUDP
WebRTC datachannel library and server

[Echo server demo](https://www.vektor.space/webudprtt.html) (Chrome, Firefox, Safari 11+)

The library implements a minimal subset of WebRTC to achieve unreliable and out of order UDP transfer for browser clients. The core library (Wu) is platform independent. Refer to WuHostEpoll or WuHostNode for usage.

## Building
```bash
mkdir build && cd build
cmake ..
make
```

### Host platforms
* Linux (epoll)
* Node.js ```-DWITH_NODE=ON```

### Issues
* Firefox doesn't connect to a server running on localhost. Bind a different interface.
