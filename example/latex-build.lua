-- run with:  TMG_LIB_PATH=../lib tmg latex-build.lua
-- '%' starts a comment in LaTeX, so use another template delimiter.
tmg_delim = "@@"

local L = require("latex")

-- 1) everything from Lua tables
local doc = L.document{
  class = "article", opts = { "11pt", "a4paper" },
  packages = { "amsmath", "booktabs", { "geometry", "margin=2cm" } },
  title = "Hello from Lua", author = "tmg", date = false,

  L.section("Intro",
    "Plain strings are escaped: 50% & $5 #1 a_b.",
    " Inline math: ", L.m"E = mc^2", ".",
    L.itemize{ "one", "two", L.enumerate{ "nested a", "nested b" } }),

  L.section({ "Data", label = "sec:data" },
    L.table{ caption = "Results", label = "tab:res", cols = "lrr",
             header = { "Name", "V1", "V2" },
             rows   = { { "a", 1, 2 }, L.midrule, { "b", 3, 4 } } },
    "See Table ", L.ref"tab:res", "."),
}
assert(L.write(doc, "latex-doc.tex"))

-- 2) hand-written LaTeX template with generated parts
assert(tmg.render("latex-demo.tmg", "latex-demo.tex"))

-- 3) optionally build PDFs (needs latexmk or pdflatex)
-- print(L.compile("latex-doc.tex"))
