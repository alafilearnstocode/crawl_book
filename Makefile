CXX      := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -O2
TARGET   := orderbook
HEADERS  := order.hpp price_level.hpp order_book.hpp

.PHONY: build run clean

build: $(TARGET)

$(TARGET): main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ main.cpp

run: build
	./$(TARGET)

clean:
	rm -f $(TARGET) *.o
