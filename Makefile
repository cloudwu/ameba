CC= gcc
CCFLAGS= --shared -fPIC
LUA_INC= /usr/local/include
LUA_LIB= /usr/local/bin
LUA_A= -llua


default:
	@echo Usage $(MAKE) windows/unix

windows:
	$(MAKE) lib NAME="dll"

unix:
	$(MAKE) lib NAME="so"

OUT= ameba.$(NAME)

lib : ameba.c
	$(CC) -I$(LUA_INC) -L$(LUA_LIB) $(CCFLAGS) $(LUA_A) $^ -o $(OUT)