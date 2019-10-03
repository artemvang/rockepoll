include config.mk


SRC = server.c utils.c io.c handler.c
OBJ = ${SRC:.c=.o}


all: options server


options:
	@echo server build options
	@echo "CFLAGS  = ${CFLAGS}"
	@echo "CPPFLAGS  = ${CPPFLAGS}"
	@echo "LDLAGS  = ${LDLAGS}"
	@echo "CC      = ${CC}"


.c.o:
	${CC} -c ${CFLAGS} $<


${OBJ}: config.mk


server: ${OBJ}
	${CC} -o $@ ${OBJ}


clean:
	rm -f server ${OBJ}


.PHONY: all options
