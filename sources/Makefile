# Modified 2026 by T. Steinmann (Rodent IV libification fork)

 #define the directory for the executable file
BINDIR = /usr/bin

#define the directory for the data files
DATADIR = /usr/share/rodentIV

# define the C compiler to use
CC = g++

# the fork's public header (include/rodent/engine.h), used by src/engine.cpp
INCLUDES = -I../include

# define the compile-time flags
CFLAGS = -g -w -Wfatal-errors -pipe -DNDEBUG -O3 -fno-rtti -finline-functions -fprefetch-loop-arrays -DBOOKPATH=$(DATADIR) $(INCLUDES)
C1FLAGS = -g -w -Wfatal-errors -pipe -DWRITEDEBUGFILE -DBOOKPATH=$(DATADIR) $(INCLUDES)

# define the link options
LDFLAGS = -s -lm
LD1FLAGS = -lm

# define output name and settings file
EXENAME= ../mac/rodentIV

default: build

build:
	@echo "Type make help for additional options"
	$(CC) $(LDFLAGS) $(CFLAGS) -std=c++14 src/*.cpp -o $(EXENAME)

clean:
	rm -f $(EXENAME)

install:
	mkdir -p -m 755 $(BINDIR)
	mkdir -p -m 755 $(DATADIR)
	cp $(EXENAME) $(BINDIR)
	cp $(CONFIGFILE) $(DATADIR)
	find -name '*.txt' -type f -exec cp '{}' $(DATADIR) \;
	cp -r ../books $(DATADIR)
	cp -r ../personalities $(DATADIR)
	chmod 755 $(BINDIR)/$(EXENAME)
	chmod -R 644 $(DATADIR)/*

help:
	@echo ""
	@echo "To compile RodentIV type:"
	@echo ""
	@echo "make build		> Compile Rodent IV"
	@echo "make install		> Install Rodent IV (root privileges required)"