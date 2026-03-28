EXE=htproxy

$(EXE): main.c proxy.h
	cc -Wall -pthread -o $@ $<

format:
	clang-format -style=file -i *.c

clean:
	rm -f $(EXE)
