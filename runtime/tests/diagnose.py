"""Hermetic Python runtime/runfiles relocation fixture."""

import json
import os
import sys

print(json.dumps([os.environ["CYBER_PATH"], sys.argv[1:]]))
