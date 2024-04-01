CXX      = g++
CPPFLAGS = -Wall -g -O2 -std=c++14
ifdef USE_AWSSDK
CPPFLAGS := $(CPPFLAGS) -DUSE_AWSSDK
endif
TARGET   = srt-live-reflect
DISTRO_ID := $(shell . /etc/os-release; echo $$ID)
UNAME_M  := $(shell uname -m)
ifeq ($(DISTRO_ID),amzn)
	ifeq ($(UNAME_M),x86_64)
		INCDIR   = -I./src -I../vcpkg/installed/x64-linux/include
		LIBDIR   = -L../vcpkg/installed/x64-linux/lib/
		LIBS     = /usr/lib64/libz.so.1
	else ifeq ($(UNAME_M),aarch64)
		echo "Unsupported architecture."
		false
	endif
else ifeq ($(DISTRO_ID),ubuntu)
	ifeq ($(UNAME_M),x86_64)
		INCDIR   = -I./src -I../vcpkg/installed/x64-linux/include
		LIBDIR   = -L../vcpkg/installed/x64-linux/lib/
		LIBS     = /usr/lib/x86_64-linux-gnu/libz.so.1
	else ifeq ($(UNAME_M),aarch64)
		INCDIR   = -I./src -I../vcpkg/installed/arm64-linux/include
		LIBDIR   = -L../vcpkg/installed/arm64-linux/lib/
		LIBS     = /usr/lib/aarch64-linux-gnu/libz.so.1
	endif
endif
LIBS     := $(LIBS) -lsrt
LIBS     := $(LIBS) -lboost_thread
LIBS     := $(LIBS) -lboost_json
LIBS     := $(LIBS) -lboost_filesystem
LIBS     := $(LIBS) -lboost_log
LIBS     := $(LIBS) -lboost_log_setup
LIBS     := $(LIBS) -lboost_chrono
ifdef USE_AWSSDK
LIBS     := $(LIBS) -laws-cpp-sdk-s3
LIBS     := $(LIBS) -laws-cpp-sdk-core
LIBS     := $(LIBS) -laws-crt-cpp
LIBS     := $(LIBS) -laws-c-mqtt
LIBS     := $(LIBS) -laws-c-event-stream
LIBS     := $(LIBS) -laws-c-s3
LIBS     := $(LIBS) -laws-c-auth
LIBS     := $(LIBS) -laws-c-http
LIBS     := $(LIBS) -laws-c-io
LIBS     := $(LIBS) -laws-c-compression
LIBS     := $(LIBS) -laws-c-cal
LIBS     := $(LIBS) -laws-c-sdkutils
LIBS     := $(LIBS) -laws-checksums
LIBS     := $(LIBS) -laws-c-common
LIBS     := $(LIBS) -ls2n
endif
LIBS     := $(LIBS) -lcurl
LIBS     := $(LIBS) -lssl
LIBS     := $(LIBS) -lcrypto
SRCDIR   = ./src
OBJDIR   = ./obj
BINDIR   = ./bin
SRCS     = $(wildcard $(SRCDIR)/*.cpp)
OBJS     = $(addprefix $(OBJDIR)/,$(notdir $(SRCS:.cpp=.o)))


all: prepare $(TARGET)

$(TARGET): $(OBJS)
	case "$(DISTRO_ID)" in \
	ubuntu) \
		$(CXX) $(CPPFLAGS) -o $(BINDIR)/$@ $^ $(LIBDIR) $(LIBS) -lz; \
		;; \
	amzn) \
		$(CXX) $(CPPFLAGS) -o $(BINDIR)/$@ $^ $(LIBDIR) $(LIBS); \
		;; \
	*) \
		echo "Unsupported distribution." \
		;; \
	esac

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CPPFLAGS) $(INCDIR) -c $< -o $@

prepare:
	mkdir -p $(BINDIR)
	mkdir -p $(OBJDIR)

clean:
	rm -f $(OBJDIR)/*.o
	rm -f $(BINDIR)/$(TARGET)
