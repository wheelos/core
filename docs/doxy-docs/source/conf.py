#!/usr/bin/env python3
# -*- coding: utf-8 -*-

html_theme = "sphinx_rtd_theme"

extensions = ["breathe", "recommonmark"]
breathe_projects = {"wheelos_core": "../xml"}
breathe_default_project = "wheelos_core"
source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}
master_doc = "index"
project = "wheelos_core"
copyright = "2024, WheelOS contributors"
author = "WheelOS contributors"

#html_logo = 'quantstack-white.svg'

exclude_patterns = []
highlight_language = "c++"
pygments_style = "sphinx"
todo_include_todos = False
htmlhelp_basename = "wheelos_core"
