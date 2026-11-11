EE_BIN = granny.elf
EE_OBJS = main.o

EE_INCS += -I$(GSKIT)/include
EE_LDFLAGS += -L$(GSKIT)/lib

EE_LIBS = -lpad -lgskit_toolkit -lgskit -ldmakit -ldebug -lpng -lz

all: $(EE_BIN)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
