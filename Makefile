EE_BIN = granny.elf
EE_OBJS = main.o
EE_LIBS = -ldebug

all: $(EE_BIN)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
