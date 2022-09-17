#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := bt_spp_acceptor_demo

BUILD_DIR_BASE := /home/msp/bis/work/builds/espd

COMPONENT_ADD_INCLUDEDIRS := components/include

include $(IDF_PATH)/make/project.mk
