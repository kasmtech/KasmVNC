#!/bin/bash

# clear previous build
rm -rf /build/*

# build webpack
npm run build
# remove node stuff from directory
rm -rf node_modules/
# copy all to build dir
cp -R ./* /build/

# remove unneccesary files
cd /build
rm *.md
rm AUTHORS
rm vnc.html
rm vnc_lite.html
