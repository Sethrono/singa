###############################################################################
# Configuration for folders and Flags
###############################################################################
# g++ location, should support c++11, tested with 4.8.1
CXX := /home/wangwei/install/bin/g++
# header folder for system and external libs
INCLUDE_DIRS := include/ /home/wangwei/install/include
# lib folder for system and external libs
LIBRARY_DIRS := /home/wangwei/install/lib64 /home/wangwei/install/lib
# folder for compiled file
BUILD_DIR := build

CXXFLAGS := -Wall -g -std=c++11 $(foreach includedir, $(INCLUDE_DIRS), -I$(includedir))
LIBRARYS := glog gflag
LDFLAGS := $(foreach librarydir, $(LIBRARY_DIRS), -L$(librarydir)) \
					$(foreach library, $(LIBRARYS), -l$(library))

TEST_LIBRARYS = $(LIBRARYS) gtest
TEST_LDFLAGS = $(LDFLAGS) -lgtest

FILTER_SRCS := src/coordinator/coordinator.cc \
							src/worker/worker.cc \
							src/main.cc \
							src/utils/start_deamon.cc \
							src/utils/global_cotext.cc \
							src/model/row_param.cc
LAPIS_SRCS := $(shell find src/ ! -name "test_*.cc" -name "*.cc")
LAPIS_SRCS :=$(filter-out $(FILTER_SRCS), $(LAPIS_SRCS))
FILTER_HDRS := include/coordinator/coordinator.h \
							include/worker/worker.h \
							include/utils/global_cotext.h \
							include/model/row_param.h
LAPIS_HDRS := $(shell find include/ -name "*.h")
LAPIS_HDRS := $(filter-out $(FILTER_HDRS), $(LAPIS_HDRS))

TEST_MAIN := src/test/test_main.cc
TEST_SRCS := $(shell find src/test/ -name "test_*.cc")
TEST_SRCS :=$(filter-out $(TEST_MAIN), $(TEST_SRCS))

PROTOS := $(shell find src/proto/ -name "*.proto")

###############################################################################
# Configuation for object files
###############################################################################
LAPIS_OBJS := $(addprefix $(BUILD_DIR)/, $(LAPIS_SRCS:.cc=.o))
TEST_OBJS := $(addprefix $(BUILD_DIR)/, $(TEST_SRCS:.cc=.o))
TEST_BINS := $(addprefix $(BUILD_DIR)/bin/, $(TEST_SRCS:.cc=.bin))


###############################################################################
# Build
###############################################################################

test: folders proto lapis.a lapis.so $(TEST_BINS)

folders:
	@ mkdir -p $(foreach obj, $(LAPIS_OBJS), $(dir $(obj)))
	@ mkdir -p $(foreach obj, $(TEST_OBJS), $(dir $(obj)))
	@ mkdir -p $(foreach bin, $(TEST_BINS), $(dir $(bin)))
	@echo

proto: $(PROTOS)
	protoc --proto_path=src/proto --cpp_out=src/proto $(PROTOS)
	mkdir -p include/proto/
	cp src/proto/*.pb.h include/proto/
	@echo

lapis.a: $(LAPIS_OBJS)
	ar rcs lapis.a $(LAPIS_OBJS)
	@echo

lapis.so: $(LAPIS_OBJS)
	$(CXX) -shared -o lapis.so $(LAPIS_OBJS) $(CXXFLAGS) $(LDFLAGS)
	@echo

$(LAPIS_OBJS): proto $(LAPIS_HDRS)

$(LAPIS_OBJS): $(BUILD_DIR)/%.o : %.cc
	$(CXX) $< $(CXXFLAGS) -c -o $@
	@echo

$(TEST_BINS): $(BUILD_DIR)/bin/src/test/%.bin: $(BUILD_DIR)/src/test/%.o
	$(CXX) $(TEST_MAIN) $< -o $@ $(CXXFLAGS) $(TEST_LDFLAGS)

$(BUILD_DIR)/src/test/%.o: src/test/%.cc
	$(CXX) $< $(CXXFLAGS) -c -o $@
