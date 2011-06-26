CFLAGS		+= -Wall
OBJS		:= vrctl.o util.o

vrctl: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f $(OBJS) vrctl
