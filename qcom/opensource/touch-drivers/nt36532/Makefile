# SPDX-License-Identifier: GPL-2.0-only

ccflags-y += -I$(srctree)/drivers/input/touchscreen

obj-$(CONFIG_TOUCHSCREEN_NT36532_SPI) += nt36532_touch.o

nt36532_touch-y := nt36xxx.o \
	nt36xxx_fw_update.o \
	nt36xxx_ext_proc.o \
	nt36xxx_mp_ctrlram.o
