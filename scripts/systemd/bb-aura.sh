#!/bin/bash

PREFIX=/usr/local/AuraUAS
SOURCE=/home/aura/Source/aura-core
echo "starting aura autopilot with prefex=$PREFIX"

# nice range is -20 to 20 with -20 being the highest priority.  -10 should be
# a good balanced number.  -20 could be a little selfish, but that might be
# exactly what we want.
NICE=-20

nice -n $NICE $PREFIX/bin/aura --python_path $SOURCE/src --config $PREFIX/config > /var/log/aura.log
