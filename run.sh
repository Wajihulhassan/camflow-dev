#!/bin/bash
make clean
rm -rf ./build/
echo "WUH preparing"
make prepare
echo "WUH comfiging"
make config_travis
echo "WUH compiling"
make compile
echo "WUH installing"
make install
