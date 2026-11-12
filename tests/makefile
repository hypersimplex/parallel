CXX = clang++
COMMON_FLAGS = -std=c++23 -Wall -Wextra -I"../"
DEBUG_FLAGS  = $(COMMON_FLAGS) -g -O0
ASAN_FLAGS   = $(COMMON_FLAGS) -g -O1 -fno-omit-frame-pointer -fsanitize=address,leak
TSAN_FLAGS   = $(COMMON_FLAGS) -g -O1 -fno-omit-frame-pointer -fsanitize=thread
TSAN_ENV     = TSAN_OPTIONS=detect_deadlocks=0

# ============================================================================
# Source & Binary Target Definitions
# ============================================================================

# 1. Coarse-Grained Closed-Addressing Hash Table
HT_SRC    = test_ht_closed_addr_lock_coarse_gained.cpp
HT_BIN    = test_ht_closed_addr_lock_coarse_gained.o
HT_ASAN   = test_ht_closed_addr_lock_coarse_gained_asan.o
HT_TSAN   = test_ht_closed_addr_lock_coarse_gained_tsan.o

# 2. Fine-Grained Lock-Coupling List Set
LIST_SRC  = test_listset_lock_fine_grained.cpp
LIST_BIN  = test_listset_lock_fine_grained.o
LIST_ASAN = test_listset_lock_fine_grained_asan.o
LIST_TSAN = test_listset_lock_fine_grained_tsan.o

.PHONY: all test asan tsan clean \
        ht ht-test ht-asan ht-tsan test-ht asan-ht tsan-ht \
        listset listset-test listset-asan listset-tsan test-listset asan-listset tsan-listset

# ============================================================================
# Global Test Rules (All Implementations)
# ============================================================================

all: test asan tsan

test: ht-test listset-test
asan: ht-asan listset-asan
tsan: ht-tsan listset-tsan

# ============================================================================
# Selective Rules: Hash Table (ht_closed_addr_lock_coarse_gained)
# ============================================================================

ht: ht-test ht-asan ht-tsan

ht-test test-ht:
	$(CXX) $(DEBUG_FLAGS) $(HT_SRC) -o $(HT_BIN)
	./$(HT_BIN)

ht-asan asan-ht:
	$(CXX) $(ASAN_FLAGS) $(HT_SRC) -o $(HT_ASAN)
	./$(HT_ASAN)

ht-tsan tsan-ht:
	$(CXX) $(TSAN_FLAGS) $(HT_SRC) -o $(HT_TSAN)
	$(TSAN_ENV) ./$(HT_TSAN)

# ============================================================================
# Selective Rules: Fine-Grained List Set (listset_lock_fine_grained)
# ============================================================================

listset: listset-test listset-asan listset-tsan

listset-test test-listset:
	$(CXX) $(DEBUG_FLAGS) $(LIST_SRC) -o $(LIST_BIN)
	./$(LIST_BIN)

listset-asan asan-listset:
	$(CXX) $(ASAN_FLAGS) $(LIST_SRC) -o $(LIST_ASAN)
	./$(LIST_ASAN)

listset-tsan tsan-listset:
	$(CXX) $(TSAN_FLAGS) $(LIST_SRC) -o $(LIST_TSAN)
	$(TSAN_ENV) ./$(LIST_TSAN)

# ============================================================================
# Cleanup
# ============================================================================

clean:
	rm -f *.o
