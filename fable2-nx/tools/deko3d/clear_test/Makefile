#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET		:=	deko3d_clear
BUILD		:=	build
SOURCES		:=	source

ARCH		:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS		:=	-g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS		+=	$(INCLUDE) -D__SWITCH__
CXXFLAGS	:=	$(CFLAGS) -std=gnu++17 -fno-exceptions -fno-rtti
ASFLAGS		:=	-g $(ARCH)
LDFLAGS		=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS		:=	-ldeko3d -lnx
LIBDIRS		:=	$(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o)
export OFILES	:=	$(OFILES_SRC)
export INCLUDE	:=	$(foreach dir,$(LIBDIRS),-I$(dir)/include)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export LD	:=	$(CXX)

.PHONY: all clean

all: | $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else
.PHONY: all

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).nro
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf	:	$(OFILES)
$(OUTPUT).nacp	:
	@nacptool --create "$(TARGET)" "Fable2Port" "0.1.0" $@

-include $(DEPENDS)
endif
