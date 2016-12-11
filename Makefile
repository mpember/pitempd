#################################################################################
#DEBUG	= -g -O0
DEBUG	= -O3
CC	= gcc
INCLUDE	= -I/usr/local/include
CFLAGS	= $(DEBUG) -Wall $(INCLUDE) -Winline -pipe

LDFLAGS	= -L/usr/local/lib
LDLIBS   = -lwiringPi -lcurl -lpthread -lm

SRC	=	pitempd.c test.c dht_logger.c
OBJ	=	$(SRC:.c=.o)
BINS =	$(SRC:.c=)
prefix=/usr/local

all:	$(BINS)

minini: minini/minIni.c

pitempd: minini
	@$(CC) -o $@ pitempd.c minini/minIni.c $(LDFLAGS) $(LDLIBS)

logger:
	@$(CC) -o $@ dht_logger.c $(LDFLAGS) $(LDLIBS)

test: minini
	@$(CC) -o $@ test.c minini/minIni.c $(LDFLAGS) $(LDLIBS)

install: pitempd
	install -C -g 0 -o 0 -m 0755 pitempd $(prefix)/sbin
	install -C -g 0 -o 0 -m 0755 pitempd.monit /etc/monit/conf.d/pitempd

clean:
	@echo "[Clean]"
	@rm -f $(OBJ) *~ core tags $(BINS)
