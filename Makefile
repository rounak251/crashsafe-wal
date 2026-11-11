# Targets:
#   make test        unit and integration tests under AddressSanitizer and UBSan
#   make crashcheck  enumerate every crash state of the two seeded traces
#   make mutants     build each seeded bug and check that crashcheck catches it
#   make bench       put throughput and recovery time (QUICK=1 for a smoke run)
#   make loc         non-test C++ line count
#   make blind       regression set of blind mutants (scripts/blind_mutants.py)

CXX ?= c++
STD := -std=c++20
WARN := -Wall -Wextra -Wpedantic
INC := -Iinclude
OPT ?= -O2
SAN ?=
EXTRA ?=
BUILD ?= build/release

CXXFLAGS := $(STD) $(WARN) $(INC) $(OPT) $(SAN) $(EXTRA) -MMD -MP
LDFLAGS := $(SAN)

LIB_SRC := $(wildcard src/*.cpp)
LIB_OBJ := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(LIB_SRC))
TEST_SRC := $(wildcard tests/*.cpp)
TEST_OBJ := $(patsubst tests/%.cpp,$(BUILD)/tests-obj/%.o,$(TEST_SRC))

.PHONY: test crashcheck mutants bench loc blind clean tools run-tests run-crashcheck

test:
	$(MAKE) BUILD=build/test OPT=-O1 SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g" run-tests

crashcheck:
	$(MAKE) BUILD=build/release run-crashcheck

mutants:
	scripts/run_mutants.sh

bench:
	QUICK=$(QUICK) BUILD=build/release scripts/bench.sh

loc:
	scripts/loc.sh

blind:
	python3 scripts/blind_mutants.py --root "$(CURDIR)"

tools:
	$(MAKE) BUILD=build/release build/release/wal_inspect build/release/trace_dump build/release/kv

clean:
	rm -rf build

run-tests: $(BUILD)/tests
	./$(BUILD)/tests

run-crashcheck: $(BUILD)/crashcheck
	./$(BUILD)/crashcheck --json bench/crashcheck.json --points bench/crashcheck-points.tsv
	scripts/recount.sh bench/crashcheck-points.tsv bench/crashcheck.json

$(BUILD)/obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/tests-obj/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/tools/%.o: tools/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/tests: $(LIB_OBJ) $(TEST_OBJ)
	$(CXX) $(LDFLAGS) $^ -o $@

$(BUILD)/crashcheck: $(LIB_OBJ) $(BUILD)/tools/crashcheck_main.o
	$(CXX) $(LDFLAGS) $^ -o $@

$(BUILD)/bench: $(LIB_OBJ) $(BUILD)/tools/bench_main.o
	$(CXX) $(LDFLAGS) $^ -o $@

$(BUILD)/wal_inspect: $(LIB_OBJ) $(BUILD)/tools/wal_inspect.o
	$(CXX) $(LDFLAGS) $^ -o $@

$(BUILD)/trace_dump: $(LIB_OBJ) $(BUILD)/tools/trace_dump.o
	$(CXX) $(LDFLAGS) $^ -o $@

$(BUILD)/kv: $(LIB_OBJ) $(BUILD)/tools/kv_main.o
	$(CXX) $(LDFLAGS) $^ -o $@

-include $(LIB_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(BUILD)/tools/crashcheck_main.d $(BUILD)/tools/bench_main.d \
	$(BUILD)/tools/wal_inspect.d $(BUILD)/tools/trace_dump.d $(BUILD)/tools/kv_main.d
