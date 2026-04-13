#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

PGB_SRCDIR = $(APPSDIR)/plugins/pgb
PGB_OBJDIR = $(BUILDDIR)/apps/plugins/pgb

PGB_SRC := $(call preprocess, $(PGB_SRCDIR)/SOURCES)
PGB_OBJ := $(call c2obj, $(PGB_SRC))

OTHER_SRC += $(PGB_SRC)

ROCKS += $(PGB_OBJDIR)/pgb.rock

$(PGB_OBJDIR)/pgb.rock: $(PGB_OBJ)
