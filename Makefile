CXX      := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -O2
SANFLAGS := -std=c++20 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
HEADERS  := order.hpp price_level.hpp order_book.hpp
BINS     := orderbook stress_test benchmark

.PHONY: build test stress asan bench clean

build: $(BINS)

orderbook: main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ main.cpp

stress_test: stress_test.cpp reference_book.hpp book_audit.hpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ stress_test.cpp

benchmark: benchmark.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ benchmark.cpp

# Scenario driver + randomized differential test against the reference engine.
test: orderbook stress_test
	./orderbook
	./stress_test

stress: stress_test
	./stress_test

# Stress test under AddressSanitizer + UBSan (fewer seeds; sanitizers are slow).
asan: stress_test.cpp reference_book.hpp book_audit.hpp $(HEADERS)
	$(CXX) $(SANFLAGS) -o stress_test_asan stress_test.cpp
	./stress_test_asan 30 20000

bench: benchmark
	./benchmark

clean:
	rm -f $(BINS) stress_test_asan *.o
	rm -rf *.dSYM
