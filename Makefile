###############################################################################
# © 2015 Ola Liljedahl
# Makefile designed by Wonderful Void
# Supports multiple targets and auto-generated dependencies
################################################################################

###############################################################################
# Project specific definitions
################################################################################

#Name of directory and also Dropbox source tar file
DIRNAME = mclanproxy
#List of executable files to build
TARGETS = mclanproxy
#List object files for each target
OBJECTS_mclanproxy = mclanproxy.o

#Customizable compiler and linker flags
GCCTARGET =
ifneq ($(DEBUG),yes)
DEFINE += -DNDEBUG#disable assertion
endif
CCFLAGS += -std=c99
CCFLAGS += -g -ggdb -W -Wall -Wextra
ifneq ($(DEBUG),yes)
CCFLAGS += -O2
endif
CXXFLAGS += -fno-exceptions
LDFLAGS += -g -ggdb -fno-exceptions
LIBS +=

#Where to find the source files
VPATH += .

#Default to non-verbose mode (echo command lines)
VERB = @

#Location of object and other derived/temporary files
OBJDIR = obj#Must not be .

###############################################################################
# Make actions (phony targets)
################################################################################

.PHONY : default all clean tags etags dropbox

default:
	@echo "Make targets:"
	@echo "all         build all targets ($(TARGETS))"
	@echo "clean       remove derived files"
	@echo "tags        generate vi tags file"
	@echo "etags       generate emacs tags file"
	@echo "dropbox     archive source files in ~/Dropbox/$(DIRNAME).tgz"

all : $(TARGETS)

#Make sure we don't remove current directory with all source files
ifeq ($(OBJDIR),.)
$(error invalid OBJDIR=$(OBJDIR))
endif
ifeq ($(TARGETS),.)
$(error invalid TARGETS=$(TARGETS))
endif
clean:
	@echo "--- Removing derived files"
	$(VERB)-rm -rf $(OBJDIR) $(TARGETS) tags TAGS perf.data perf.data.old

tags :
	$(VERB)ctags -R .

etags :
	$(VERB)ctags -e -R .

dropbox :
	$(VERB)-rm -f ~/Dropbox/$(DIRNAME).tgz
	$(VERB)cd ..; tar cvfz ~/Dropbox/$(DIRNAME).tgz $(addprefix $(DIRNAME)/,$(filter-out $(TARGETS) $(OBJDIR) tags TAGS perf.data perf.data.old,$(wildcard *)))

################################################################################
# Setup tool commands and flags
################################################################################

#Defaults to be overriden by compiler makefragment
CCOUT = -o $@
ASOUT = -o $@
LDOUT = -o $@

ifneq ($(GCCTARGET),)
#Some experimental cross compiling support
#GCCROOT = /opt/gcc-linaro-arm-linux-gnueabihf-4.7-2013.02-01-20130221_linux
#GCCLIB = $(GCCROOT)/lib/gcc/$(GCCTARGET)/4.7.3
GCCROOT = /opt/gcc-linaro-arm-linux-gnueabihf-4.7-2012.12-20121214_linux
GCCSETUP = PATH=$(GCCROOT)/bin:$(GCCROOT)/$(GCCTARGET)/bin:/bin:/usr/bin
CC = $(GCCSETUP) $(GCCROOT)/bin/$(GCCTARGET)-gcc
CXX = $(GCCSETUP) $(GCCROOT)/bin/$(GCCTARGET)-g++
LD = $(GCCSETUP) $(GCCROOT)/bin/$(GCCTARGET)-g++
else
#Native compilation
ifeq ($(CLANG),yes)
CC = clang
CXX = clang++
AS = as
LD = clang++
else
CC = gcc
CXX = g++
AS = as
LD = g++
endif
endif
BIN2C = bin2c

#Important compilation flags
CCFLAGS += -c -MMD -MP

################################################################################
# Post-process some variables and definitions, generate dependencies
################################################################################

CCFLAGS += $(DEFINE) $(INCLUDE)
#Generate list of all object files (for all targets)
override OBJECTS := $(addprefix $(OBJDIR)/,$(foreach var,$(TARGETS),$(OBJECTS_$(var))))
#Generate target:objects dependencies for all targets
$(foreach target,$(TARGETS),$(eval $(target) : $$(addprefix $$(OBJDIR)/,$$(OBJECTS_$(target)))))
#Special dependency for object files on object directory
$(OBJECTS) : | $(OBJDIR)

################################################################################
# Build recipes
################################################################################

$(OBJDIR) :
	$(VERB)mkdir -p $(OBJDIR)

#Keep intermediate pcap C-files
.PRECIOUS : $(OBJDIR)/%_pcap.c

$(OBJDIR)/%_pcap.o : $(OBJDIR)/%_pcap.c
	@echo "--- Compiling $<"
	$(VERB)$(CC) $(CCFLAGS) $(CCOUT) $<

$(OBJDIR)/%_pcap.c : %.pcap
	@echo "--- Generating $@"
	$(VERB)$(BIN2C) -n $(notdir $(basename $@)) -o $@ $<

$(OBJDIR)/%.o : %.cc
	@echo "--- Compiling $<"
	$(VERB)$(CXX) $(CXXFLAGS) $(CCFLAGS) $(CCOUT) $<

$(OBJDIR)/%.o : %.c
	@echo "--- Compiling $<"
	$(VERB)$(CC) $(CCFLAGS) $(CCOUT) $<

$(OBJDIR)/%.o : %.s
	@echo "--- Compiling $<"
	$(VERB)$(AS) $(ASFLAGS) $(ASONLYFLAGS) $(ASOUT) $<

$(OBJDIR)/%.o : %.S
	@echo "--- Compiling $<"
	$(VERB)$(CC) $(CCFLAGS) $(addprefix $(ASPREFIX),$(ASFLAGS)) $(CCOUT) $<

$(TARGETS) :
	@echo "--- Linking $@ from $<"
	$(VERB)$(LD) $(LDFLAGS) $(LDOUT) $(addprefix $(OBJDIR)/,$(OBJECTS_$@)) $(GROUPSTART) $(LIBS) $(GROUPEND) $(LDMAP)

################################################################################
# Include generated dependencies
################################################################################

-include $(patsubst %.o,%.d,$(OBJECTS))
# DO NOT DELETE
