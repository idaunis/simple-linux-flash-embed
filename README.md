simple-linux-flash-embed
========================

## Introduction

A simple GTK example to embed flash in standalone Linux applications

## How to Compile

	gcc -Wall -DXP_UNIX=1 -DMOZ_X11=1 -fPIC -g -o player player.c `pkg-config --libs --cflags gtk+-2.0`

## License

Copyright 2012 Ivan Daunis.

Licensed under the MIT License
