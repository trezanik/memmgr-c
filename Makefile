CC=gcc
CCFLAGS= -Wall -g -std=c99 -D_GNU_SOURCE -lpthread

ROOTd = ~/projects/memmgr-c-poc
BINd = $(ROOTd)/bin
OBJd = $(ROOTd)/obj
SRCd = $(ROOTd)/src
BIN_NAME = memmgr-poc

$(OBJd)/%.o : %.c
	$(CC) -c $< -o $(OBJd)/$@


.SILENT : $(BIN_NAME)
$(BIN_NAME): $(SRCd)/*.c $(SRCd)/*.h
	$(CC) $(CCFLAGS) $? -o $(BINd)/$@
	chmod +x $(BINd)/$(BIN_NAME)
	echo "making '$(BIN_NAME)' is complete"


.SILENT : clean
.PHONY : clean
clean:
	rm -f $(OBJd)/*.o
	rm -f $(SRCd)/*~
	rm -f $(BINd)/*
