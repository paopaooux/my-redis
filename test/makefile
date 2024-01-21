# 编译器
CC=g++
#编译选项
CFLAGS=-Wall -Wextra -O2 -g 

# $@目标  $<第一个依赖 $^所有依赖
test:test_avl test_offset test_heap

test_avl:test_avl.cpp
	$(CC) $(CFLAGS) -o $@ $<

test_offset:test_offset.cpp
	$(CC) $(CFLAGS) -o $@ $<

test_heap:test_heap.cpp
	$(CC) $(CFLAGS) -o $@ $<


#声明clean为伪目标
.PHONY:clean

clean:
	rm -f test_avl test_offset test_heap



