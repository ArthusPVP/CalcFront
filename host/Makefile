# PC build of the simulation core (for testing)
CC ?= gcc
CFLAGS = -O2 -g -Wall -Wextra -DHOST -fsanitize=address,undefined -fno-sanitize-recover=undefined
SRC = main.c $(wildcard ../src/sim/*.c)
cfhost: $(SRC) ../src/sim/sim.h
	$(CC) $(CFLAGS) -o $@ $(SRC)
clean:
	rm -f cfhost
