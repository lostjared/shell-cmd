OBJ=$(patsubst %.cpp,%.o,$(wildcard *.cpp))
PROGRAM=shell-cmd
CFLAGS= -std=c++20 -Wall -pedantic -O3
LDFLAGS=

%.o: %.cpp
	$(CXX) $(CFLAGS) $< -c -o $@

$(PROGRAM): $(OBJ)
	$(CXX) -o $(PROGRAM) $(OBJ) $(LDFLAGS)

all: $(PROGRAM)

.PHONY: clean
clean:
	rm -f $(PROGRAM) *.o
install:
	cp $(PROGRAM) /usr/local/bin
