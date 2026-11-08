_DIR   := .build
FF_VER := shared
_EXE   := untrunc
IS_RELEASE := 0
LIBUI_STATIC := 0

# make switching between ffmpeg versions easy
TARGET := $(firstword $(MAKECMDGOALS))
ifeq ($(TARGET), $(_EXE)-33)
	FF_VER := 3.3.9
	EXE := $(TARGET)
else ifeq ($(TARGET), $(_EXE)-34)
	FF_VER := 3.4.5
	EXE := $(TARGET)
else ifeq ($(TARGET), $(_EXE)-341)
	FF_VER := 3.4.1
	EXE := $(TARGET)
else ifeq ($(TARGET), $(_EXE)-41)
	FF_VER := 4.1
	EXE := $(TARGET)
else ifeq ($(TARGET), $(_EXE)-60)
	FF_VER := 6.0
	EXE := $(TARGET)
else ifeq ($(TARGET), $(_EXE)-70)
	FF_VER := 7.0
	EXE := $(TARGET)
else ifeq ($(TARGET), $(_EXE)-71)
	FF_VER := 7.1
	EXE := $(TARGET)
else ifeq ($(TARGET), $(_EXE)-80)
	FF_VER := 8.0
	EXE := $(TARGET)
else ifeq ($(TARGET), $(_EXE)-81)
	FF_VER := 8.1
	EXE := $(TARGET)
endif

ifeq ($(OS),Windows_NT)
	_OS := Windows
else
	_OS := $(shell uname)
endif

FF_DIR := ffmpeg-$(FF_VER)

ifeq ($(FF_VER), shared)
	CXXFLAGS += -isystem/usr/include/ffmpeg
	LDFLAGS += -lavformat -lavcodec -lavutil
else
	CXXFLAGS += -isystem./$(FF_DIR)
	LDFLAGS += -L$(FF_DIR)/libavformat -lavformat
	LDFLAGS += -L$(FF_DIR)/libavcodec -lavcodec
	LDFLAGS += -L$(FF_DIR)/libavutil -lavutil
	#LDFLAGS += -L$(FF_DIR)/libswscale/ -lswresample
	#LDFLAGS += -L$(FF_DIR)/libavresample -lavresample
	#LDFLAGS += -lz -lbz2 -lX11 -lva -lva-drm -lva-x11 -llzma
	LDFLAGS += -lpthread -ldl

	ifeq ($(_OS), Darwin)
		# deprioritize implicit -I /usr/local/include to -isystem so local FFmpeg headers take precedence
		CXXFLAGS += -isystem/usr/local/include
		LDFLAGS += -liconv
	endif
endif

CXXFLAGS += -std=c++17 -D_FILE_OFFSET_BITS=64

ifeq ($(IS_RELEASE), 1)
	CXXFLAGS += -O3
	LDFLAGS += -s
else
	CXXFLAGS += -g
endif

VER = $(shell test -d .git && command -v git >/dev/null && echo "v`git rev-list --count HEAD`-`git describe --always --dirty --abbrev=7`")
CPPFLAGS += -MMD -MP
CPPFLAGS += -DUNTR_VERSION=\"$(VER)\"
USE_GCH := 0

EXE ?= $(_EXE)
DIR := $(_DIR)_$(FF_VER)
PCH := src/pch.h
PCH_OBJ := $(PCH:%=$(DIR)/%.gch)
PCH_INC := $(PCH_OBJ:%.gch=%)
SRC := $(wildcard src/*.cpp src/avc1/*.cpp src/hvc1/*.cpp src/ai/*.cpp src/ai/local/*.cpp src/ai/external/*.cpp src/ai/lmstudio/*.cpp src/ai/onnx/*.cpp)
OBJ := $(SRC:%.cpp=$(DIR)/%.o)
DEP := $(OBJ:.o=.d)

# Pruebas unitarias (make test): enlazan todo menos src/main.o.
TEST_EXE := $(_EXE)-tests
OBJ_TEST := $(DIR)/tests/test_main.o
DEP_TEST := $(OBJ_TEST:.o=.d)

SRC_GUI := $(wildcard src/gui/*.cpp)
OBJ_GUI := $(SRC_GUI:%.cpp=$(DIR)/%.o)
DEP_GUI := $(OBJ_GUI:.o=.d)

ifeq ($(TARGET), $(_EXE)-gui)
	LDFLAGS += -lui -lpthread

	machine = $(shell $(CXX) -dumpmachine)
	ifneq (,$(findstring mingw,$(machine)))
		LDFLAGS += -Wl,--subsystem,windows
		# GetOpenFileNameW / CommDlgExtendedError (seleccion multiple).
		LDFLAGS += -lcomdlg32
		ifeq ($(LIBUI_STATIC), 1)
			OBJ_GUI += $(DIR)/src/gui/win_resources.o
			LDFLAGS += -lpthread -luser32 -lkernel32 -lusp10 -lgdi32 -lcomctl32 -luxtheme -lmsimg32 -lcomdlg32 -ld2d1 -ldwrite -lole32 -loleaut32 -loleacc
		endif
	endif
endif

ifeq ($(_OS), Darwin)
	NJOBS = $(shell echo $$(( $$(sysctl -n hw.physicalcpu) - 1 )) )
else
	NJOBS = $(shell echo $$(( $$(nproc) / 3 )) )
endif
ifeq ($(NJOBS), 0)
	NJOBS = 1
endif

FF_CONFIG_FLAGS := --disable-doc \
	--disable-everything --enable-decoders --enable-parser=hevc --disable-vdpau --enable-demuxers --enable-protocol=file \
	--disable-avdevice --disable-swresample --disable-swscale --disable-avfilter \
	--disable-xlib --disable-vaapi --disable-zlib --disable-bzlib --disable-lzma \
	--disable-audiotoolbox --disable-videotoolbox

ifneq ($(FF_VER), shared)
	FF_MAJOR_VER := $(word 1, $(subst ., ,$(FF_VER)))

	# Flags that have been removed
	ifeq ($(shell test $(FF_MAJOR_VER) -lt 4; echo $$?),0)
		FF_CONFIG_FLAGS += --disable-vda
	endif
	ifeq ($(shell test $(FF_MAJOR_VER) -lt 8; echo $$?),0)
		FF_CONFIG_FLAGS += --disable-postproc
	endif

	# Flags that have been added
	ifeq ($(shell test $(FF_MAJOR_VER) -gt 6; echo $$?),0)
		FF_CONFIG_FLAGS += --disable-libdrm
	endif
	ifeq ($(_OS), Darwin)
		ifeq ($(shell test $(FF_MAJOR_VER) -lt 5; echo $$?),0)
			FF_CONFIG_FLAGS += --extra-cflags="-Wno-error=incompatible-function-pointer-types"
		endif
	endif
endif

#$(info $$OBJ is [${OBJ}])
#$(info $$OBJ_GUI is [${OBJ_GUI}])
$(shell mkdir -p $(dir $(OBJ_GUI)) 2>/dev/null)
$(shell mkdir -p $(DIR)/tests $(DIR)/src/avc1 $(DIR)/src/hvc1 $(DIR)/src/ai $(DIR)/src/ai/local $(DIR)/src/ai/external $(DIR)/src/ai/lmstudio $(DIR)/src/ai/onnx 2>/dev/null)

# ONNX Runtime se detecta por dlopen en el proveedor (onnx_provider.cpp);
# en Linux hace falta -ldl. En Windows no existe -ldl.
# Se detecta por el compilador (MinGW) y no por $(OS): dentro de
# una shell de MSYS2 (y en el CI) $(OS) no esta definida y _OS
# vale "MINGW64_NT-...", asi que antes se anadia -ldl y el enlace
# fallaba con "cannot find -ldl".
ifeq (,$(findstring mingw,$(shell $(CXX) -dumpmachine)))
LDFLAGS += -ldl
endif

CURL := $(shell command -v curl 2>/dev/null)
download = $(if $(CURL),curl -L -o $(1) $(2),wget -q --show-progress -O $(1) $(2))

.PHONY: all clean force dist dist-linux dist-windows test


all: $(EXE)

$(FF_DIR)/configure:
	@#read -p "Press [ENTER] if you agree to build ffmpeg-${FF_VER} now.. " input
	@echo "(info) downloading $(FF_DIR) ..."
	$(call download,/tmp/$(FF_DIR).tar.xz,https://www.ffmpeg.org/releases/$(FF_DIR).tar.xz)
	tar xf /tmp/$(FF_DIR).tar.xz

$(FF_DIR)/config.asm: | $(FF_DIR)/configure
	@echo "(info) please wait ..."
	cd $(FF_DIR); ./configure $(FF_CONFIG_FLAGS)

$(FF_DIR)/libavcodec/libavcodec.a: | $(FF_DIR)/config.asm
	cat $(FF_DIR)/Makefile
	$(MAKE) -C $(FF_DIR) -j$(NJOBS)
ifeq ($(_OS), Darwin)
	# avoid collision with C++ <version> header on case-insensitive FS
	-mv $(FF_DIR)/VERSION $(FF_DIR)/VERSION.bak
endif

$(FF_DIR):
ifneq ($(FF_VER), shared)
$(FF_DIR): | $(FF_DIR)/libavcodec/libavcodec.a
endif

print_info: | $(FF_DIR)
	@echo untrunc: $(VER)
	@echo ffmpeg: $(FF_VER)
	@echo

$(EXE): print_info $(OBJ)
	$(CXX) $(filter-out $<,$^) $(LDFLAGS) -o $@

$(EXE)-gui: print_info $(filter-out $(DIR)/src/main.o, $(OBJ)) $(OBJ_GUI)
	$(CXX) $(filter-out $<,$^) $(LDFLAGS) -o $@

$(TEST_EXE): print_info $(filter-out $(DIR)/src/main.o, $(OBJ)) $(OBJ_TEST)
	$(CXX) $(filter-out $<,$^) $(LDFLAGS) -o $@

test: $(TEST_EXE)
	./$(TEST_EXE)

$(DIR)/%/win_resources.o: %/win_resources.rc
	windres.EXE $< $@

# rebuild src/common.o on new version/CPPFLAGS
$(DIR)/cpp_flags: force
	@echo '$(CPPFLAGS)' | cmp -s - $@ || echo '$(CPPFLAGS)' > $@
$(DIR)/src/common.o: $(DIR)/cpp_flags

$(PCH_OBJ): $(PCH)
	$(CXX) $(CXXFLAGS) -x c++-header -o $@ -c $<

ifeq ($(USE_GCH),1)
$(DIR)/%.o: %.cpp $(PCH_OBJ)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Winvalid-pch -include $(PCH_INC) -o $@ -c $<
else
$(DIR)/%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ -c $<
endif

-include $(DEP)
-include $(DEP_GUI)
-include $(DEP_TEST)

clean:
	$(RM) -r $(DIR)
	$(RM) $(EXE)
	$(RM) $(EXE)-gui
	$(RM) $(TEST_EXE)

# Fase 3.2 — Paquete portable (ZIP). La lista de archivos y las
# verificaciones viven en empaquetar.sh (una sola fuente de verdad);
# el Makefile solo compila lo necesario y lo llama.
#   make dist-linux   -> ZIP de prueba para Linux (CLI; la GUI es Windows,
#                        en Linux no compila por falta de libui)
#   make dist-windows -> ZIP para Windows (en MSYS2 MINGW64; compila CLI
#                        + GUI y recoge las DLL con ntldd)
#   make dist         -> alias de dist-linux
dist: dist-linux

dist-linux: $(EXE)
	./empaquetar.sh linux

dist-windows: $(EXE) $(EXE)-gui
	./empaquetar.sh windows

