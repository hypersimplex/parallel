CXX = clang++
CXXFLAGS = -std=c++23 -g -O1 -fno-omit-frame-pointer -I"../"
SRC = test_ht_closed_addr_lock_coarse_gained.cpp
TARGET = test_ht_closed_addr_lock_coarse_gained.o
TARGET_ASAN = test_ht_closed_addr_lock_coarse_gained_asan.o
TARGET_TSAN = test_ht_closed_addr_lock_coarse_gained_tsan.o

.PHONY: all test asan tsan clean

all: test asan tsan

test:
	$(CXX) -std=c++23 -g -O0 -I"../" $(SRC) -o $(TARGET)
	./$(TARGET)

asan:
	$(CXX) $(CXXFLAGS) -fsanitize=address,leak $(SRC) -o $(TARGET_ASAN)
	./$(TARGET_ASAN)

tsan:
	$(CXX) $(CXXFLAGS) -fsanitize=thread $(SRC) -o $(TARGET_TSAN)
	TSAN_OPTIONS=detect_deadlocks=0 ./$(TARGET_TSAN)

clean:
	rm -f *.o

