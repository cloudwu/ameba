all : ameba.dll

ameba.dll : ameba.c
	gcc -o $@ --shared -g -Wall -I/usr/local/include -L/usr/local/bin -llua52 $^

