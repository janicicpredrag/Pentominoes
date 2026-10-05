CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
OBJS = pieces.o game.o ai.o display.o terminal.o main.o

pentomino: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp *.hpp
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f $(OBJS) pentomino

.PHONY: clean
