# Makefile for the V4L2 OpenGL Real-time Viewer

# --- Variables ---

# Compiler
CC = gcc

# Target executable name
TARGET = v4l2_gl

# Source files (add more .c files here if your project grows)
SRCS = v4l2_gl.c

# Object files (automatically generated from SRCS)
OBJS = $(SRCS:.c=.o) 

# Compiler flags:
# -Wall:      Enable all standard warnings
# -Wextra:    Enable extra warnings
# -g:         Generate debugging information (for gdb)
# -O2:        Optimization level 2 (for performance)
# You might use -g for development and -O2 for release.
CFLAGS = -Wall -Wextra -g -O2

# Core graphics libraries
GRAPHICS_LIBS = -lglut -lGL -lGLU -lusb-1.0

# Viture SDK library
VITURE_LIB = 3rdparty/lib/libviture_one_sdk_static.a

LIBS = $(LDFLAGS) $(GRAPHICS_LIBS) 

# Standard command for removing files
RM = rm -f




# --- Rules ---

# The default goal is 'all', which builds the target executable.
# The .PHONY directive tells make that 'all' is not a file.
.PHONY: all
all: $(TARGET)

# Rule to link the object files into the final executable.
# The executable depends on all the object files.
$(TARGET): $(OBJS)
	@echo "==> Linking..."
	$(CC) -o $(TARGET) $(OBJS) $(VITURE_LIB) $(LIBS)
	@echo "==> Build complete: ./"$(TARGET)

# Pattern rule to compile .c files into .o files.
# For any .o file, make will find the corresponding .c file
# and use this recipe to build it.
%.o: %.c
	@echo "==> Compiling $<..."
	$(CC) $(CFLAGS) -c -o $@ $<

# The 'clean' rule removes all generated files.
# .PHONY tells make that 'clean' is not a file.
.PHONY: clean
clean:
	@echo "==> Cleaning up..."
	$(RM) $(TARGET) $(OBJS)
	@echo "==> Done."
