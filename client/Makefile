CXX=g++
CXXFLAGS=-std=gnu++17 -O3 -Wall -Wextra
LFLAGS=-lpthread
TARGET=client

CLIENT_MODE ?= 1
CXXFLAGS += -DCLIENT_MODE=$(CLIENT_MODE)
ifeq ($(CLIENT_MODE),1)
    EXECUTABLE_NAME = client
    CXXFLAGS += -DWAIT_FOR_TCP_CONNECTION=1
else
    EXECUTABLE_NAME = server
    CXXFLAGS += -DWAIT_FOR_TCP_CONNECTION=0
endif

CAN_FORWARDER_MODE ?= 0
CXXFLAGS += -DCAN_FORWARDER_MODE=$(CAN_FORWARDER_MODE)

# Check if the DEBUG environment variable is set.
DEBUG ?= 0
ifeq ($(DEBUG),1)
    CXXFLAGS += -DDEBUG
else
    CXXFLAGS += -DNDEBUG
endif

# Explicitly tell Make about the targets.
.PHONY: all clean

# Rebuild all objects when the Makefiles change.
$(EXECUTABLE_NAME): $(MAKEFILE_LIST)

all: $(EXECUTABLE_NAME)

$(EXECUTABLE_NAME): $(TARGET).cpp
	$(CXX) $(<) -o $(EXECUTABLE_NAME) $(CXXFLAGS) $(LFLAGS)

clean:
	$(RM) *.o *~ client server
