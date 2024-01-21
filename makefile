# 编译器
CC=g++
#编译选项
CFLAGS=-Wall -Wextra -O2 -g -pthread

# 源文件路径
SRC_DIR = src
INCLUDE_DIR = include
# 目标文件路径
OBJ_DIR = obj

# 生成的可执行文件名
TARGET = server client
TEST = test_avl test_offset test_heap
OBJ_server = $(OBJ_DIR)/server.o $(OBJ_DIR)/wrap.o $(OBJ_DIR)/hashtable.o $(OBJ_DIR)/avl.o $(OBJ_DIR)/zset.o $(OBJ_DIR)/common.o $(OBJ_DIR)/list.o $(OBJ_DIR)/heap.o $(OBJ_DIR)/thread_pool.o
OBJ_client = $(OBJ_DIR)/client.o
# 所有源文件
SRCS = $(wildcard $(SRC_DIR)/*.cpp)
# 所有头文件
HEADERS = $(wildcard $(INCLUDE_DIR)/*.h)

# 自动生成依赖关系
DEPS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.d,$(SRCS))
-include $(DEPS)

# 链接目标文件生成可执行文件
target: $(TARGET)

# 生成目标文件和依赖关系文件
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -MMD -c -o $@ $<
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -MM -MT $@ -MF $(OBJ_DIR)/$*.d $<

# 生成可执行文件
server: $(OBJ_server)
	$(CC) $(CFLAGS) -o $@ $^

client: $(OBJ_client)
	$(CC) $(CFLAGS) -o $@ $^

# 清理目标文件和可执行文件
clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(TEST)
