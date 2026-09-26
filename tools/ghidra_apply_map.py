# Apply the v1.0 race.exe linker map (out/symbols.csv, from tools/extract_map.py) to a Ghidra program.
# @category ViperPort
# @runtime Jython
#
# Run from Ghidra's Script Manager with race.exe open. Labels go on with their MANGLED names, so
# Ghidra's Microsoft demangler (Analysis > Demangler Microsoft, on by default) turns each into a full
# signature -- class, calling convention, parameter and return types. Functions are created where
# Ghidra hasn't found them, and the source object file goes in each function's plate comment.
import csv
from ghidra.program.model.symbol import SourceType

path = askFile("symbols.csv from tools/extract_map.py", "Apply").getAbsolutePath()
st = currentProgram.getSymbolTable()
fm = currentProgram.getFunctionManager()
made = named = 0
with open(path) as fh:
    for row in csv.DictReader(fh):
        addr = toAddr(int(row["va"], 16))
        try:
            st.createLabel(addr, row["name"], SourceType.IMPORTED)
            named += 1
        except Exception as ex:
            print("label %s %s: %s" % (row["va"], row["name"], ex))
        if row["kind"] == "function" and row["section"] == ".text":
            f = fm.getFunctionAt(addr)
            if f is None:
                f = createFunction(addr, None)
                made += 1
            if f is not None:
                src = row["library"] + ":" + row["object"] if row["library"] not in ("root", "crt") else row["object"]
                f.setComment("from " + src)
print("named %d addresses, created %d functions" % (named, made))
