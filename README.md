simple-linux-flash-embed
========================

A simple linux flash embed

## How to Compile

	gcc -Wall -DXP_UNIX=1 -DMOZ_X11=1 -fPIC -g -o player player.c `pkg-config --libs --cflags gtk+-2.0`

## License

Copyright 2012 Ivan Daunis.

Licensed under the MIT License
