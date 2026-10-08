#!/usr/bin/env python3
"""ue3_dis.py <package> <Class> [Function]: disassemble UnrealScript functions
of a Drakengard 3 package (e.g. game/disc/PS3_GAME/USRDIR/SQEX03GAME/COOKEDPS3/
SQEX03GAME.XXX Sqex03GameHUDOptionRoot)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ue3 import dis

if __name__ == '__main__':
    dis.main()
