# Compiler and flags
CXX      ?= g++
CXXFLAGS ?= -O3 -march=native -flto -Wall -Wextra -std=c++17 -fno-exceptions -fno-rtti -s
LDFLAGS  := -lX11

# Install paths
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

TARGET   := mothl
SRC      := main.cpp

.PHONY: all clean install uninstall run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)
	@strip $(TARGET)
	@echo "Built and stripped $(TARGET) successfully."

# Run launcher
run: $(TARGET)
	./$(TARGET)

# Force-rescan and run
rescan: $(TARGET)
	./$(TARGET) -r

# Install to system (default: /usr/local/bin)
install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	@echo "Installed to $(DESTDIR)$(BINDIR)/$(TARGET)"

# Remove from system
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	@echo "Uninstalled $(DESTDIR)$(BINDIR)/$(TARGET)"

# Clean build artifacts and tmpfs cache
clean:
	rm -f $(TARGET)
	rm -f /dev/shm/xlauncher_*.cache
	@echo "Cleaned build binary and tmpfs cache."