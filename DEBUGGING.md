# Debugging
In the case where KasmVNC crashes and a backtrace is produced. Developers need a way to make the backtrace useful for debugging. This document covers where the backtrace is logged and how to use a debug symbol package and gdb to gain more information from the backtrace, such as filename, function name, and line number.

## Test Symbolization
If you want to induce a crash to test that you can symbolize a backtrace you can start KasmVNC and then issue the following command.

    killall -SEGV Xvnc

This will cause KasmVNC to terminate with a backtrace similar to the following. You will find the backtrace in the log file at $HOME/.vnc/$HOSTNAME:$DISPLAY.log where HOME is your user's profile path, HOSTNAME is the hostname of the system, and DISPLAY is the assigned display number for the session.

    (EE) 
    (EE) Backtrace:
    (EE) 0: /usr/bin/Xvnc (xorg_backtrace+0x4d) [0x5e48dd]
    (EE) 1: /usr/bin/Xvnc (0x400000+0x1e8259) [0x5e8259]
    (EE) 2: /lib/x86_64-linux-gnu/libpthread.so.0 (0x7f5a57ef6000+0x12980) [0x7f5a57f08980]
    (EE) 3: /lib/x86_64-linux-gnu/libc.so.6 (epoll_wait+0x57) [0x7f5a552eca47]
    (EE) 4: /usr/bin/Xvnc (ospoll_wait+0x37) [0x5e8d07]
    (EE) 5: /usr/bin/Xvnc (WaitForSomething+0x1c3) [0x5e2813]
    (EE) 6: /usr/bin/Xvnc (Dispatch+0xa7) [0x597007]
    (EE) 7: /usr/bin/Xvnc (dix_main+0x36e) [0x59b1fe]
    (EE) 8: /lib/x86_64-linux-gnu/libc.so.6 (__libc_start_main+0xe7) [0x7f5a551ecbf7]
    (EE) 9: /usr/bin/Xvnc (_start+0x2a) [0x46048a]
    (EE) 
    (EE) Received signal 11 sent by process 17182, uid 0
    (EE) 
    Fatal server error:
    (EE) Caught signal 11 (Segmentation fault). Server aborting
    (EE) 

## Debug Symbol Package
In order to make use of this backtrace, you will need to symbolize the backtrace. Each build of KasmVNC produced by our pipelines comes with a corresponding debug symbol package that can be downloaded. You need two pieces of information to get the package, the git commit ID of KasmVNC and the MD5 sum of Xkasmvnc binary on your system. 

The git commit ID can be found using Xvnc -version:

    ubuntu@matt-dev-vm-1:~$ Xvnc -version
    Xvnc KasmVNC 0.9.94002f39917dca0ad82ac0c29a75c723b538b32b - built Feb 17 2022 15:21:01
    Copyright (C) 1999-2018 KasmVNC Team and many others (see README.me)
    See http://kasmweb.com for information on KasmVNC.
    Underlying X server release 12010000, The X.Org Foundation

The MD5 sum can be obtained using the md5sum command:

    md5sum /usr/bin/Xkasmvnc
    57ee7028239c5a737c0aeee4a34138c3  /usr/bin/Xkasmvnc
    
With these two pieces of information, you can get the debug symbol package at https://kasmweb-build-artifacts.s3.amazonaws.com/kasmvnc/crashpad/[COMMITID]/[MD5SUM]/kasmvncserver-dbgsym.deb, in the above example it would be.

[https://kasmweb-build-artifacts.s3.amazonaws.com/kasmvnc/crashpad/94002f39917dca0ad82ac0c29a75c723b538b32b/57ee7028239c5a737c0aeee4a34138c3/kasmvncserver-dbgsym.deb](https://kasmweb-build-artifacts.s3.amazonaws.com/kasmvnc/crashpad/94002f39917dca0ad82ac0c29a75c723b538b32b/57ee7028239c5a737c0aeee4a34138c3/kasmvncserver-dbgsym.deb "https://kasmweb-build-artifacts.s3.amazonaws.com/kasmvnc/crashpad/94002f39917dca0ad82ac0c29a75c723b538b32b/57ee7028239c5a737c0aeee4a34138c3/kasmvncserver-dbgsym.deb")

Use wget or curl to download the debug symbol package and then install it.

    wget  https://kasmweb-build-artifacts.s3.amazonaws.com/kasmvnc/crashpad/94002f39917dca0ad82ac0c29a75c723b538b32b/57ee7028239c5a737c0aeee4a34138c3/kasmvncserver-dbgsym.deb
    sudo dpkg -i kasmvncserver-dbgsym.deb

## Symbolize a Backtrace
With the KasmVNC binary and debug symbol package installed on the system, you can use gdb or addr2line to get more information, such as the filename, function name, and line number that the backtrace line is referring to. 

Here is a single line from a backtrace. The following example shows how to retrieve additional information that can help with debugging the crash.
(EE) 8: /usr/bin/Xvnc (0x400000+0x13e674) [**0x53e674**]

    echo info line ***0x53eaaa** | gdb -q /usr/bin/Xkasmvnc
    (gdb) Line 223 of "/src/common/network/webudp/Wu.cpp" starts at address 0x53e674 <WuClientSendPendingDTLS(WuClient*, Wu const*, Wu const*)+68>

The following script will search the provide file for a backtrace and symbolize the entire backtrace.

    #!/bin/bash
      
    FILENAME=$1
    grep "(EE)" $FILENAME | while read -r line ; do
            BACKTRACE=$(echo $line | grep -Po '\[[0-9a-f]x[a-f0-9]{1,}' | sed -r 's#\[##')
            echo $line
            if ! [ -z $BACKTRACE ] ; then
                    SYMBOLIZED=$(echo "info line *${BACKTRACE}" | gdb /usr/bin/Xkasmvnc | grep "(gdb)" | grep -vP "\(gdb\)\s*quit$")
                    echo "    ${SYMBOLIZED}"
            fi
    done

Using this script on the above example backtrace produces the following output.

    ubuntu@hostname-1:~$ bash symbolize.sh ~/.vnc/hostname-1\:10.log
    (EE)
    (EE) Backtrace:
    (EE) 0: /usr/bin/Xvnc (xorg_backtrace+0x4d) [0x5e48dd]
        (gdb) Line 130 of "backtrace.c" starts at address 0x5e48dd <xorg_backtrace+77> and ends at 0x5e4900 <xorg_backtrace+112>.
    (EE) 1: /usr/bin/Xvnc (0x400000+0x1e8259) [0x5e8259]
        (gdb) Line 138 of "osinit.c" starts at address 0x5e8259 <OsSigHandler+41> and ends at 0x5e8275 <OsSigHandler+69>.
    (EE) 2: /lib/x86_64-linux-gnu/libpthread.so.0 (0x7f5a57ef6000+0x12980) [0x7f5a57f08980]
        (gdb) No line number information available for address 0x7f5a57f08980
    (EE) 3: /lib/x86_64-linux-gnu/libc.so.6 (epoll_wait+0x57) [0x7f5a552eca47]
        (gdb) No line number information available for address 0x7f5a552eca47
    (EE) 4: /usr/bin/Xvnc (ospoll_wait+0x37) [0x5e8d07]
        (gdb) Line 643 of "ospoll.c" starts at address 0x5e8d07 <ospoll_wait+55> and ends at 0x5e8d09 <ospoll_wait+57>.
    (EE) 5: /usr/bin/Xvnc (WaitForSomething+0x1c3) [0x5e2813]
        (gdb) Line 210 of "WaitFor.c" starts at address 0x5e2813 <WaitForSomething+451> and ends at 0x5e2819 <WaitForSomething+457>.
    (EE) 6: /usr/bin/Xvnc (Dispatch+0xa7) [0x597007]
        (gdb) Line 421 of "dispatch.c" starts at address 0x596ffb <Dispatch+155> and ends at 0x59700b <Dispatch+171>.
    (EE) 7: /usr/bin/Xvnc (dix_main+0x36e) [0x59b1fe]
        (gdb) Line 278 of "main.c" starts at address 0x59b1fe <dix_main+878> and ends at 0x59b203 <dix_main+883>.
    (EE) 8: /lib/x86_64-linux-gnu/libc.so.6 (__libc_start_main+0xe7) [0x7f5a551ecbf7]
        (gdb) No line number information available for address 0x7f5a551ecbf7
    (EE) 9: /usr/bin/Xvnc (_start+0x2a) [0x46048a]
        (gdb) No line number information available for address 0x46048a <_start+42>
    (EE)
    (EE) Received signal 11 sent by process 17182, uid 0
    (EE)
    (EE) Caught signal 11 (Segmentation fault). Server aborting
    (EE)

