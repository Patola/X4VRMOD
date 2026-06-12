CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -lopenxr_loader -lm

xr2x4: xr2x4.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f xr2x4

.PHONY: clean
