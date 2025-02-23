#!/bin/sh

# clear previous build
rm -rf /build/*

# build webpack
npm run build

# copy all to build dir
cp -R ./dist/* /build/
