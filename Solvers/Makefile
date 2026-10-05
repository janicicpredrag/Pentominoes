# Builds the two C++ solvers (portable optimisation, no machine-specific flags).
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall

all: solver_drafting solver_nodrafting

solver_drafting: drafting/solver_drafting.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

solver_nodrafting: nodrafting/solver_nodrafting.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f solver_drafting solver_nodrafting

.PHONY: all clean
