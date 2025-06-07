# Makefile for the V4L2 OpenGL Real-time Viewer

# --- Variables ---

# Compiler
CC = gcc

# Target executable name
TARGET = v4l2_gl
TARGET_TEST = test_viture

# Source files (add more .c files here if your project grows)
SRCS = v4l2_gl.c viture_connection.c
SRCS_TEST = test_viture.c viture_connection.c

# Object files (automatically generated from SRCS)
OBJS = $(SRCS:.c=.o)
OBJS_TEST = $(SRCS_TEST:.c=.o)

# Compiler flags:
# -Wall:      Enable all standard warnings
# -Wextra:    Enable extra warnings
# -g:         Generate debugging information (for gdb)
# -O2:        Optimization level 2 (for performance)
# You might use -g for development and -O2 for release.
CFLAGS = -Wall -Wextra -g -O2

# Core graphics libraries
GRAPHICS_LIBS = -lglut -lGL -lGLU -lusb-1.0

# HIDAPI library
HIDAPI_LIB = -lhidapi-libusb
PTHREAD_LIB = -lpthread

# Viture SDK library (not used by test_viture directly, but viture_connection.o might have been compiled with it if it were part of its sources)
# For test_viture, we only need viture_connection.o, which itself doesn't use VITURE_LIB.
VITURE_LIB = 3rdparty/lib/libviture_one_sdk_static.a

LIBS = $(LDFLAGS) $(GRAPHICS_LIBS) $(HIDAPI_LIB) $(PTHREAD_LIB)
LIBS_TEST = $(LDFLAGS) $(HIDAPI_LIB) $(PTHREAD_LIB)

# Standard command for removing files
RM = rm -f




# --- Rules ---

# The default goal is 'all', which builds the target executable.
# The .PHONY directive tells make that 'all' is not a file.
.PHONY: all test
all: $(TARGET)

test: $(TARGET_TEST)

# Rule to link the object files into the final executable.
# The executable depends on all the object files.
$(TARGET): $(filter v4l2_gl.o viture_connection.o, $(OBJS))
	@echo "==> Linking $(TARGET)..."
	$(CC) -o $(TARGET) $(filter v4l2_gl.o viture_connection.o, $(OBJS)) $(VITURE_LIB) $(LIBS)
	@echo "==> Build complete: ./"$(TARGET)

# Rule to link the test_viture executable
$(TARGET_TEST): $(filter test_viture.o viture_connection.o, $(OBJS_TEST))
	@echo "==> Linking $(TARGET_TEST)..."
	$(CC) -o $(TARGET_TEST) $(filter test_viture.o viture_connection.o, $(OBJS_TEST)) $(LIBS_TEST)
	@echo "==> Build complete: ./"$(TARGET_TEST)


# Pattern rule to compile .c files into .o files.
# For any .o file, make will find the corresponding .c file
# and use this recipe to build it.
# This rule will be used for all .c files including test_viture.c and viture_connection.c
%.o: %.c
	@echo "==> Compiling $<..."
	$(CC) $(CFLAGS) -I. -c -o $@ $< # Added -I. for viture_connection.h

# The 'clean' rule removes all generated files.
# .PHONY tells make that 'clean' is not a file.
.PHONY: clean
clean:
	@echo "==> Cleaning up..."
	$(RM) $(TARGET) $(TARGET_TEST) $(OBJS) $(OBJS_TEST)
	@echo "==> Done."
