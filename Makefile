# Compiler and flags
CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++11
TARGET = can_bridge

# Source files
SOURCES = can_bridge.cpp
OBJECTS = $(SOURCES:.cpp=.o)

# Default target
all: $(TARGET)

# Build the executable
$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJECTS)
	@echo "Build complete: $(TARGET)"

# Compile source files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(TARGET)
	@echo "Clean complete"

# Install (copy to /usr/local/bin - requires sudo)
install: $(TARGET)
	install -m 0755 $(TARGET) /usr/local/bin/
	@echo "Installed to /usr/local/bin/$(TARGET)"

# Uninstall
uninstall:
	rm -f /usr/local/bin/$(TARGET)
	@echo "Uninstalled from /usr/local/bin/"

# Run the program (requires sudo for CAN access)
run: $(TARGET)
	sudo ./$(TARGET)

.PHONY: all clean install uninstall run
