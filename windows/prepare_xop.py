#!/usr/bin/env python3
"""Compatibility entry for the shared POSIX XOP preparation tool."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).resolve().parents[1] / 'tools/prepare_xop.py'), run_name='__main__')
