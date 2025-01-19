# 编译器
CXX = g++

# 编译选项
CXXFLAGS = -std=c++23 -Wall -Wextra -O2

# 目标可执行文件
TARGET = Server

# 源文件
SRCS = ../Server.cpp

# 默认目标
all: $(TARGET)

# 生成可执行文件
$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^

# 清理生成的文件
clean:
	rm -f $(TARGET)

# 伪目标
.PHONY: all clean