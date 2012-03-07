CXX=clang++
CXXFLAGS=-I/usr/lib/llvm-3.0/include -std=c++11 -fno-rtti

all: objcount.so

objcount.so : objcount.o
		$(CXX) -shared -o objcount.so objcount.o

objcount.o : objcount.cpp
		$(CXX) $(CXXFLAGS) -c objcount.cpp

Countable.o : Countable.cpp
		$(CXX) $(CXXFLAGS) -c Countable.cpp

test-app.pre.cpp : test-app.cpp objcount.so
		$(CXX) -std=c++11 -E test-app.cpp > test-app.pre.cpp
		$(CXX) -cc1 -load ./objcount.so -plugin objcount -std=c++11 test-app.pre.cpp

test-app.o : test-app.pre.cpp
		$(CXX) -std=c++11 -o test-app.o -c test-app.pre.cpp

test-app : Countable.o test-app.o
		$(CXX) -o test-app Countable.o test-app.o

test : test-app
		./test-app

clean:
		rm -f *.o *.so *.pre.cpp test-app
