#
# Component Makefile (for esp-idf)
#
# This Makefile should, at the very least, just include $(SDK_PATH)/make/component.mk. By default,
# this will take the sources in this directory, compile them and link them into
# lib(subdirectory_name).a in the build directory. This behaviour is entirely configurable,
# please read the SDK documents if you need to do this.
#

ifdef CONFIG_ESPHTTPD_ENABLED

COMPONENT_SRCDIRS := core espfs util
COMPONENT_ADD_INCLUDEDIRS := core espfs util include
COMPONENT_ADD_LDFLAGS := -lwebpages-espfs -llibesphttpd

COMPONENT_EXTRA_CLEAN := mkespfsimage/*

HTMLDIR := $(subst ",,$(CONFIG_ESPHTTPD_HTMLDIR))
HTMLFILES := $(shell find $(PROJECT_PATH)/$(HTMLDIR) | sed -E 's/([[:space:]])/\\\1/g')

JS_MINIFY_TOOL ?= uglifyjs
YUI-COMPRESSOR ?= $(PROJECT_PATH)/../Tools/java.exe -jar $(PROJECT_PATH)/../Tools/yuicompressor-2.4.8.jar

CFLAGS += -DFREERTOS -DESPFS_HEATSHRINK

USE_HEATSHRINK := "yes"
COMPONENT_ADD_INCLUDEDIRS += lib/heatshrink

USE_GZIP_COMPRESSION := "yes"


liblibesphttpd.a: libwebpages-espfs.a

# mkespfsimage will compress html, css, svg and js files with gzip by default if enabled
# override with -g cmdline parameter
webpages.espfs: $(HTMLFILES) mkespfsimage/mkespfsimage
ifeq ("$(CONFIG_ESPHTTPD_USEUGLIFYJS)","y")
	echo "Using uglifyjs"
	rm -rf html_compressed;
	cp -r $(PROJECT_PATH)/$(HTMLDIR) html_compressed;
	echo "Compressing javascript assets with uglifyjs"
	for file in `find html_compressed -type f -name "*.js"`; do $(JS_MINIFY_TOOL) $$file -c -m -o $$file; done
	awk "BEGIN {printf \" compression ratio was: %.2f%%\\n\", (`du -b -s html_compressed/ | sed 's/\([0-9]*\).*/\1/'`/`du -b -s $(PROJECT_PATH)/$(HTMLDIR) | sed 's/\([0-9]*\).*/\1/'`)*100}"
	cd html_compressed; find . | $(COMPONENT_BUILD_DIR)/mkespfsimage/mkespfsimage > $(COMPONENT_BUILD_DIR)/webpages.espfs; cd ..;
else ifeq ("$(CONFIG_ESPHTTPD_USEYUICOMPRESSOR)","y")
	echo "Using yui-compressor"
	rm -rf html_compressed;
	cp -r $(PROJECT_PATH)/$(HTMLDIR) html_compressed;
	echo "Compressing assets with yui-compressor."
	for file in `find html_compressed -type f -name "*.js"`; do $(YUI-COMPRESSOR) --type js $$file -o $$file; done
	for file in `find html_compressed -type f -name "*.css"`; do $(YUI-COMPRESSOR) --type css $$file -o $$file; done
	echo "yui-compressor done.";
	awk "BEGIN {printf \" compression ratio was: %.2f%%\\n\", (`du -b -s html_compressed/ | sed 's/\([0-9]*\).*/\1/'`/`du -b -s $(PROJECT_PATH)/$(HTMLDIR) | sed 's/\([0-9]*\).*/\1/'`)*100}"
	cd html_compressed; find . | $(COMPONENT_BUILD_DIR)/mkespfsimage/mkespfsimage > $(COMPONENT_BUILD_DIR)/webpages.espfs; cd ..;
else
	echo "Not using uglifyjs or yui-compressor"
	cd  $(PROJECT_PATH)/$(HTMLDIR) &&  find . | $(COMPONENT_BUILD_DIR)/mkespfsimage/mkespfsimage > $(COMPONENT_BUILD_DIR)/webpages.espfs
endif

libwebpages-espfs.a: webpages.espfs
	$(OBJCOPY) -I binary -O elf32-xtensa-le -B xtensa --rename-section .data=.rodata \
		webpages.espfs webpages.espfs.o.tmp
	$(CC) -nostdlib -Wl,-r webpages.espfs.o.tmp -o webpages.espfs.o -Wl,-T $(COMPONENT_PATH)/webpages.espfs.esp32.ld
	$(AR) cru $@ webpages.espfs.o

mkespfsimage/mkespfsimage: $(COMPONENT_PATH)/espfs/mkespfsimage
	mkdir -p $(COMPONENT_BUILD_DIR)/mkespfsimage
	$(MAKE) -C $(COMPONENT_BUILD_DIR)/mkespfsimage -f $(COMPONENT_PATH)/espfs/mkespfsimage/Makefile \
		USE_HEATSHRINK="$(USE_HEATSHRINK)" USE_GZIP_COMPRESSION="$(USE_GZIP_COMPRESSION)" BUILD_DIR=$(COMPONENT_BUILD_DIR)/mkespfsimage \
		CC=$(HOSTCC)

endif
