include config.mk


SRC = server.c utils.c io.c log.c parser.c handler.c
OBJ = ${SRC:.c=.o}


all: options rockepoll


options:
	@echo server build options
	@echo "CFLAGS  = ${CFLAGS}"
	@echo "CPPFLAGS  = ${CPPFLAGS}"
	@echo "LDLAGS  = ${LDLAGS}"
	@echo "CC      = ${CC}"


config.h:
	cp config.def.h config.h


.c.o:
	${CC} -o $@ -c ${CFLAGS} $<


${OBJ}: config.mk config.h


rockepoll: ${OBJ}
	${CC} -static -o $@ ${OBJ} -lpthread


clean:
	rm -f server ${OBJ}


.PHONY: all options
